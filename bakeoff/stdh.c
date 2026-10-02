/* stdh.c - cycles and instructions of ONE (de)compression, counted around the codec call only.
 *
 *   stdh CODEC LEVEL FILE [REPS]          decode loop (compresses once, then decodes REPS times, keeps the best)
 *   CMODE=1 stdh CODEC LEVEL FILE [REPS]  the same for the compress call
 *   CODEC: lz6 | zstd | lizard (only those built in; zstd and lizard cannot be linked together: same symbols)
 *
 * Counters come from perf_event_open (user mode only), so it needs root, and the numbers are only as
 * quiet as the core: pin it (sudo taskset -c N ./stdh ...). The round trip is checked on every decode.
 * Why not `perf stat` around lzbench: for slow compressors the noise of the compression swamps the
 * decode delta (cycles even came out negative).
 *
 * Build, from a lzbench checkout that already built zstd and lizard ($Z = lz/zstd/lib, $L = lz/lizard):
 *   LZ6="lib/lz6.c lib/lz6hc.c lib/entropy/lz6seq.c lib/entropy/lz6fse.c lib/entropy/lz6rc.c lib/entropy/lz6huf.c"
 *   cc -O2 -std=gnu11 -DHAVE_ZSTD -DZSTD_MULTITHREAD=0 -I$Z -I$Z/common -Ilib -Ilib/entropy bakeoff/stdh.c \
 *      $Z/common/*.c $Z/compress/*.c $Z/decompress/*.c $Z/decompress/huf_decompress_amd64.S $LZ6 -o stdh_zstd -lm -lpthread
 *   cc -O2 -std=gnu11 -DHAVE_LIZARD -I$L -Ilib -Ilib/entropy bakeoff/stdh.c \
 *      $L/lizard_compress.c $L/lizard_decompress.c $L/entropy/*.c $LZ6 -o stdh_lizard -lm -lpthread
 */
/* dh.c CODEC LEVEL FILE [REPS] : compress once in memory, then count user cycles + instructions around the DECODE loop only
 * (perf_event_open, needs root). codec: lz6 | zstd | lizard */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>
#ifdef HAVE_ZSTD
#include <zstd.h>
#endif
#ifdef HAVE_LIZARD
#include "lizard_compress.h"
#include "lizard_decompress.h"
#endif
#include "entropy/lz6seq.h"

static int pfd(uint32_t cfg, int group)
{
    struct perf_event_attr a; memset(&a, 0, sizeof(a));
    a.type = PERF_TYPE_HARDWARE; a.size = sizeof(a); a.config = cfg; a.disabled = group < 0; a.exclude_kernel = 1; a.exclude_hv = 1;
    return (int)syscall(SYS_perf_event_open, &a, 0, -1, group, 0);
}
static uint64_t rd(int fd) { uint64_t v = 0; if (read(fd, &v, 8) != 8) return 0; return v; }

int main(int argc, char** argv)
{
    const char *codec = argv[1]; int lvl = atoi(argv[2]); const char* fn = argv[3]; int reps = argc > 4 ? atoi(argv[4]) : 12;
    FILE* f = fopen(fn, "rb"); if (!f) return 1; fseek(f, 0, SEEK_END); size_t n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *src = malloc(n), *cmp = malloc(n + n/2 + 65536), *dst = malloc(n + 65536);
    if (fread(src, 1, n, f) != n) return 2; fclose(f);
    size_t cs = 0;
    int cmode = getenv("CMODE") != NULL;
    int cnt_c = -1, cnt_i = -1; uint64_t cbest = ~0ULL, ibest = 0; size_t csz_last = 0;
    if (cmode) {
        cnt_c = pfd(PERF_COUNT_HW_CPU_CYCLES, -1); cnt_i = pfd(PERF_COUNT_HW_INSTRUCTIONS, cnt_c);
        if (cnt_c < 0 || cnt_i < 0) { fprintf(stderr, "perf_event_open failed\n"); return 4; }
#ifdef HAVE_ZSTD
        ZSTD_CCtx* zc = ZSTD_createCCtx();
#endif
        for (int r = 0; r < reps + 1; r++) {
            ioctl(cnt_c, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP); ioctl(cnt_c, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
            size_t z;
#ifdef HAVE_ZSTD
            if (!strcmp(codec, "zstd")) z = ZSTD_compressCCtx(zc, cmp, n + n/2 + 65536, src, n, lvl); else
#endif
#ifdef HAVE_LIZARD
            if (!strcmp(codec, "lizard")) z = (size_t)Lizard_compress((const char*)src, (char*)cmp, (int)n, (int)(n + n/2 + 65536), lvl); else
#endif
            z = LZ6_compress_seq((const char*)src, n, (char*)cmp, n + n/2 + 65536, lvl);
            ioctl(cnt_c, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
            csz_last = z;
            if (r == 0) continue;
            uint64_t cc = rd(cnt_c), ii = rd(cnt_i);
            if (cc < cbest) { cbest = cc; ibest = ii; }
        }
        printf("%-7s %-3d %-8s %9zu -> %9zu (%5.2f%%)  COMPRESS %7.3f cyc/B  %7.3f ins/B  IPC %.2f\n", codec, lvl, strrchr(fn,'/')?strrchr(fn,'/')+1:fn, n, csz_last, 100.0*csz_last/n, (double)cbest/n, (double)ibest/n, (double)ibest/cbest);
        return 0;
    }
#ifdef HAVE_ZSTD
    if (!strcmp(codec, "zstd")) cs = ZSTD_compress(cmp, n + n/2 + 65536, src, n, lvl); else
#endif
#ifdef HAVE_LIZARD
    if (!strcmp(codec, "lizard")) cs = (size_t)Lizard_compress((const char*)src, (char*)cmp, (int)n, (int)(n + n/2 + 65536), lvl); else
#endif
    if (strcmp(codec, "lz6")) { fprintf(stderr, "codec not built in\n"); return 6; } else cs = LZ6_compress_seq((const char*)src, n, (char*)cmp, n + n/2 + 65536, lvl);
    if (!cs
#ifdef HAVE_ZSTD
        || (!strcmp(codec, "zstd") && ZSTD_isError(cs))
#endif
       ) { fprintf(stderr, "compress failed\n"); return 3; }
    int c = pfd(PERF_COUNT_HW_CPU_CYCLES, -1), i = pfd(PERF_COUNT_HW_INSTRUCTIONS, c);
    if (c < 0 || i < 0) { fprintf(stderr, "perf_event_open failed (need root)\n"); return 4; }
    uint64_t bestc = ~0ULL, besti = 0;
    for (int r = 0; r < reps + 2; r++) {
        memset(dst, 0, 1);   /* touch */
        ioctl(c, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP); ioctl(c, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
        size_t d;
#ifdef HAVE_ZSTD
        if (!strcmp(codec, "zstd")) d = ZSTD_decompress(dst, n + 65536, cmp, cs); else
#endif
#ifdef HAVE_LIZARD
        if (!strcmp(codec, "lizard")) d = (size_t)Lizard_decompress_safe((const char*)cmp, (char*)dst, (int)cs, (int)n); else
#endif
        d = LZ6_decompress_seq((const char*)cmp, cs, (char*)dst, n + 65536);
        ioctl(c, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
        if (d != n || memcmp(src, dst, n)) { fprintf(stderr, "ROUND TRIP FAILED (%zu vs %zu)\n", d, n); return 5; }
        if (r < 2) continue;   /* warm-up */
        uint64_t cc = rd(c), ii = rd(i);
        if (cc < bestc) { bestc = cc; besti = ii; }
    }
    printf("%-7s %-3d %-8s %9zu -> %9zu (%5.2f%%)  %6.3f cyc/B  %6.3f ins/B  IPC %.2f\n", codec, lvl, strrchr(fn,'/')?strrchr(fn,'/')+1:fn, n, cs, 100.0*cs/n, (double)bestc/n, (double)besti/n, (double)besti/bestc);
    return 0;
}
