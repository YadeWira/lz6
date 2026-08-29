/* bench_seq.c - mem-to-mem speed benchmark + fuzz gate for the lz6 seq codec.
 *
 * Modes:
 *   bench_seq [--level N] file...   per-file csize / enc MB/s / dec MB/s + totals
 *   bench_seq --fuzz N [file]       push N corrupted/random streams through
 *                                   LZ6_decompress_seq into exact-size buffers
 *                                   (ASAN gate for the decode path)
 *
 * Build (mirrors programs/Makefile's lz6seq_test object set, all -O2):
 *   cc -O2 -std=c99 -I. bakeoff/bench_seq.c \
 *      lib/lz6.o lib/lz6hc.o lib/entropy/lz6fse.o lib/entropy/lz6rc.o \
 *      lib/entropy/lz6seq.o lib/entropy/lz6huf.o -lm -o bench_seq
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lib/entropy/lz6seq.h"
#include "lib/lz6frame.h"

#define TARGET_WINDOW 2.0   /* seconds of measurement per direction */
#define KB *(1u << 10)
#define MB *(1u << 20)

static int g_level = 2;
static double g_total_orig = 0.0, g_total_csize = 0.0, g_tot_dec_time_units = 0.0;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void* xmalloc(size_t n) {
    void* p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "bench_seq: out of memory\n"); exit(1); }
    return p;
}

/* run batches until >= TARGET_WINDOW elapsed; return best batch MB/s.
 * mbs_size is the byte count credited per iteration (input for enc,
 * output for dec). */
static double loop_best_mbs(int is_enc, const char* src, size_t srcSize,
                            char* dst, size_t dstCap, size_t mbs_size, size_t* out_csize) {
    double best = 0.0, elapsed = 0.0;
    unsigned long batch = 4;
    while (elapsed < TARGET_WINDOW) {
        double t0, dt, mbs;
        int n = (int)batch, i;
        t0 = now_s();
        if (is_enc) {
            for (i = 0; i < n; i++)
                *out_csize = LZ6_compress_seq(src, srcSize, dst, dstCap, g_level);
            if (!*out_csize) return -1.0;
        } else {
            for (i = 0; i < n; i++) {
                size_t d = LZ6_decompress_seq(src, srcSize, dst, dstCap);
                if (!d) return -1.0;
            }
        }
        dt = now_s() - t0;
        elapsed += dt;
        mbs = (double)n * (double)mbs_size / dt / 1e6;
        if (mbs > best) best = mbs;
        if (dt > 0.0) {
            batch = (unsigned long)((double)batch * 0.25 / dt);  /* ~0.25s batches */
            if (batch < 1) batch = 1;
        }
    }
    return best;
}

static int bench_file(const char* path, double* tot_dec_time_units) {
    FILE* f = fopen(path, "rb");
    long sz;
    unsigned char *src, *cbuf, *dbuf;
    size_t csize, dsz, cap;
    double enc_mbs, dec_mbs;
    int ok;

    if (!f) { fprintf(stderr, "bench_seq: cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return 1; }
    src = xmalloc((size_t)sz);
    if (fread(src, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return 1; }
    fclose(f);

    cap = (size_t)sz + (size_t)sz / 2 + 65536;
    cbuf = xmalloc(cap);
    dbuf = xmalloc((size_t)sz + 65536);

    csize = LZ6_compress_seq((const char*)src, (size_t)sz, (char*)cbuf, cap, g_level);
    if (!csize) { fprintf(stderr, "bench_seq: compression failed on %s\n", path); return 1; }
    dsz = LZ6_decompress_seq((const char*)cbuf, csize, (char*)dbuf, (size_t)sz + 65536);
    ok = dsz == (size_t)sz && memcmp(dbuf, src, (size_t)sz) == 0;
    if (!ok) { fprintf(stderr, "bench_seq: ROUNDTRIP FAIL on %s\n", path); return 1; }

    enc_mbs = loop_best_mbs(1, (const char*)src, (size_t)sz, (char*)cbuf, cap, (size_t)sz, &csize);
    dec_mbs = loop_best_mbs(0, (const char*)cbuf, csize, (char*)dbuf, (size_t)sz + 65536, (size_t)sz, &csize);
    if (enc_mbs < 0 || dec_mbs < 0) { fprintf(stderr, "bench_seq: bench loop failed on %s\n", path); return 1; }

    printf("%-12s %10ld -> %10zu  (%5.2f%%)  enc %8.1f MB/s  dec %8.1f MB/s  rt=OK\n",
           path, sz, csize, 100.0 * (double)csize / (double)sz, enc_mbs, dec_mbs);

    /* accumulate size-weighted reciprocal for the aggregate MB/s */
    *tot_dec_time_units += (double)sz / dec_mbs;
    g_total_orig += (double)sz;
    g_total_csize += (double)csize;

    free(src); free(cbuf); free(dbuf);
    return 0;
}

/* ---- fuzz mode ---- */
static unsigned long long rng_state = 0x9E3779B97F4A7C15ull;
static unsigned int rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (unsigned int)(rng_state >> 32);
}

/* compressible seed material: first file arg, else synthetic text */
static unsigned char* load_seed(const char* seed_path, size_t* seed_sz_out, size_t cap) {
    unsigned char* seed = NULL;
    *seed_sz_out = 0;
    if (seed_path) {
        FILE* f = fopen(seed_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long s = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (s > 0 && s < (long)cap) {
                seed = xmalloc((size_t)s);
                if (fread(seed, 1, (size_t)s, f) == (size_t)s) *seed_sz_out = (size_t)s;
                else { free(seed); seed = NULL; }
            }
            fclose(f);
        }
    }
    if (!seed) {
        static const char* txt = "the quick brown fox jumps over the lazy dog. ";
        size_t tlen = strlen(txt), i;
        size_t sz = 256 KB;
        seed = xmalloc(sz);
        for (i = 0; i < sz; i += tlen) memcpy(seed + i, txt, sz - i < tlen ? sz - i : tlen);
        *seed_sz_out = sz;
    }
    return seed;
}

static int fuzz_mode(unsigned long nstreams, const char* seed_path) {
    unsigned char *seed = NULL, *stream, *dst;
    size_t seed_sz = 0;
    unsigned long i, decoded = 0;
    size_t cbuf_cap = 4 MB;
    unsigned char* cbuf = xmalloc(cbuf_cap);

    seed = load_seed(seed_path, &seed_sz, cbuf_cap);

    for (i = 0; i < nstreams; i++) {
        size_t slen = 0, csz = 0, cap;
        int kind = (int)(rnd() % 3);
        unsigned int j, nmut;

        if (kind == 0) {
            /* fully random bytes */
            slen = 8 + rnd() % 8192;
            stream = xmalloc(slen);
            for (j = 0; j < slen; j++) stream[j] = (unsigned char)rnd();
        } else {
            /* compressed slice of the seed, then mutated and/or truncated */
            size_t off = rnd() % seed_sz;
            size_t len = 64 + rnd() % (seed_sz - off > 256 KB ? 256 KB : seed_sz - off);
            if (off + len > seed_sz) len = seed_sz - off;
            csz = LZ6_compress_seq((const char*)seed + off, len, (char*)cbuf, cbuf_cap, g_level);
            if (!csz) continue;
            slen = csz;
            stream = xmalloc(slen);
            memcpy(stream, cbuf, slen);
            if (kind == 2 && slen > 16) {
                size_t cut = 5 + rnd() % (slen - 5);
                slen = cut;
            }
            nmut = 1 + rnd() % 8;
            for (j = 0; j < nmut && slen > 0; j++) {
                size_t p = rnd() % slen;
                stream[p] = (unsigned char)rnd();
            }
        }

        /* decode into a buffer sized from the (possibly corrupt) header,
         * exactly like the CLI does: isize + 65536 slack, bounded at 1MB */
        {
            size_t isize = 0;
            if (slen >= 5) isize = (size_t)stream[1] | ((size_t)stream[2] << 8) |
                                   ((size_t)stream[3] << 16) | ((size_t)stream[4] << 24);
            cap = isize <= (1u << 20) ? isize + 65536 : (1u << 20) + 65536;
            dst = xmalloc(cap);
            /* save each stream so a crash under ASAN leaves the culprit behind */
            {
                char path[64];
                snprintf(path, sizeof(path), "/tmp/seqfuzz_last.bin");
                FILE* sf = fopen(path, "wb");
                if (sf) { fwrite(stream, 1, slen, sf); fclose(sf); }
            }
            if (LZ6_decompress_seq((const char*)stream, slen, (char*)dst, cap)) decoded++;
            free(dst);
        }
        free(stream);
    }

    printf("fuzz: %lu streams (%lu decoded), no crash\n", nstreams, decoded);
    free(cbuf); free(seed);
    return 0;
}

/* frame-level fuzz: build ONE valid seq frame over the seed, then mutate
 * envelope bytes (frame header, block headers incl. the bit31 escape,
 * payload, truncations) and push them through LZ6F_decompress — the gate
 * for the frame layer the raw-stream fuzz cannot reach. */
static int frame_fuzz_mode(unsigned long nstreams, const char* seed_path) {
    size_t seed_sz = 0;
    unsigned char* seed = load_seed(seed_path, &seed_sz, 4 MB);
    unsigned long i;

    LZ6F_preferences_t p;
    memset(&p, 0, sizeof(p));
    p.frameInfo.blockSizeID = LZ6F_max1MB;   /* multi-block: block headers in scope */
    p.frameInfo.blockCodec = LZ6F_blockCodec_seq;
    p.frameInfo.contentChecksumFlag = LZ6F_contentChecksumEnabled;
    p.compressionLevel = g_level;
    p.autoFlush = 1;
    size_t fbound = LZ6F_compressFrameBound(seed_sz, &p) + 64;
    unsigned char* frame = xmalloc(fbound);
    size_t fsz = LZ6F_compressFrame(frame, fbound, seed, seed_sz, &p);
    if (LZ6F_isError(fsz)) { fprintf(stderr, "ffuzz: frame compress failed\n"); return 1; }

    unsigned char* stream = xmalloc(fsz);
    unsigned char* dst = xmalloc(seed_sz);
    unsigned long decoded = 0;

    for (i = 0; i < nstreams; i++) {
        size_t len = fsz;
        memcpy(stream, frame, fsz);
        unsigned int nmut = 1 + rnd() % 24;
        for (unsigned int j = 0; j < nmut; j++) {
            size_t pos = rnd() % len;
            stream[pos] ^= (unsigned char)(1u << (rnd() % 8));
        }
        if (rnd() % 8 == 0) len = 1 + rnd() % fsz;   /* truncation */

        LZ6F_decompressionContext_t dctx;
        if (LZ6F_isError(LZ6F_createDecompressionContext(&dctx, LZ6F_VERSION))) return 1;
        size_t pos = 0, done = 0;
        while (pos < len) {
            size_t din = len - pos;
            size_t dout = seed_sz - done;
            size_t r = LZ6F_decompress(dctx, dst + done, &dout, stream + pos, &din, NULL);
            if (LZ6F_isError(r)) break;
            pos += din;
            done += dout;
            if (din == 0 && dout == 0) break;
        }
        LZ6F_freeDecompressionContext(dctx);
        if (done == seed_sz) decoded++;
    }

    printf("ffuzz: %lu frames (%lu decoded clean), no crash\n", nstreams, decoded);
    free(stream); free(dst); free(frame); free(seed);
    return 0;
}

int main(int argc, char** argv) {
    int i, argi, rc = 0;
    unsigned long fuzz_n = 0, ffuzz_n = 0;
    const char* fuzz_seed = NULL;
    int file_arg_start = argc;

    for (argi = 1; argi < argc; argi++) {
        if (!strcmp(argv[argi], "--level") && argi + 1 < argc) { g_level = atoi(argv[++argi]); if (g_level < 1) g_level = 1; if (g_level > 15) g_level = 15; }
        else if (!strcmp(argv[argi], "--fuzz") && argi + 1 < argc) { fuzz_n = (unsigned long)atol(argv[++argi]); }
        else if (!strcmp(argv[argi], "--ffuzz") && argi + 1 < argc) { ffuzz_n = (unsigned long)atol(argv[++argi]); }
        else if (!strcmp(argv[argi], "--fuzz-seed") && argi + 1 < argc) { fuzz_seed = argv[++argi]; }
        else if (argv[argi][0] != '-') { file_arg_start = argi; break; }
    }

    if (fuzz_n) return fuzz_mode(fuzz_n, fuzz_seed ? fuzz_seed : (file_arg_start < argc ? argv[file_arg_start] : NULL));
    if (ffuzz_n) return frame_fuzz_mode(ffuzz_n, fuzz_seed ? fuzz_seed : (file_arg_start < argc ? argv[file_arg_start] : NULL));

    if (file_arg_start >= argc) {
        fprintf(stderr, "usage: bench_seq [--level N] file...\n       bench_seq --fuzz N [seedfile]   (raw seq streams)\n       bench_seq --ffuzz N [seedfile]  (seq frames through LZ6F_decompress)\n");
        return 1;
    }

    for (i = file_arg_start; i < argc; i++) {
        double tu = 0.0;
        if (bench_file(argv[i], &tu)) rc = 1;
        g_tot_dec_time_units += tu;
    }

    if (g_total_orig > 0) {
        double ratio = 100.0 * g_total_csize / g_total_orig;
        double dec_w = g_tot_dec_time_units > 0 ? g_total_orig / g_tot_dec_time_units : 0.0;
        printf("TOTAL   %10.0f -> %10.0f  (%5.2f%%)  dec(weighted) %8.1f MB/s  score %10.1f\n",
               g_total_orig, g_total_csize, ratio, dec_w,
               dec_w > 0 ? dec_w * (g_total_orig / g_total_csize) : 0.0);
    }
    return rc;
}
