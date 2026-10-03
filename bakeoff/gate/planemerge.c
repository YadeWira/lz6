/* planemerge.c — gate test for the byte-plane merge of LZ6_decompress_seq.
 *
 * The merge (stride 2 and 4) is a 16-byte SSE2 loop plus a scalar tail. This
 * test hand-builds plane blocks from RAW lanes (lane mode 0, so no entropy
 * coder is involved), decodes each into an EXACTLY sized heap destination from
 * an EXACTLY sized heap source, and compares the output with a scalar
 * reference interleave. Under -fsanitize=address a load or store past the
 * internal plane buffer, the source or the destination hits a redzone.
 *
 * Covers every plane size 0..300 for both strides (all residues mod 16, with
 * and without whole 16-byte steps), random larger sizes up to ~200 KB,
 * destinations at every offset mod 16, and the cap checks: a destination one
 * byte short and a source one byte short must both be refused.
 *
 * Build and run it for the default and the -DLZ6SEQ_NO_DYNAMIC_BMI2 clone, and
 * once with -U__SSE2__ to cover the scalar-only merge:
 *   cc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -std=gnu11 \
 *      -Ilib -Ilib/entropy bakeoff/gate/planemerge.c lib/lz6.c lib/lz6hc.c \
 *      lib/entropy/lz6seq.c lib/entropy/lz6fse.c lib/entropy/lz6rc.c lib/entropy/lz6huf.c \
 *      -lm -o planemerge && ./planemerge
 * Usage: planemerge [random_cases_per_stride]   (default 3000; exit 0 = pass)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "entropy/lz6seq.h"

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static unsigned xrand(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (unsigned)(rng >> 32);
}

static void* xmalloc(size_t n)
{
    void* p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(2); }
    return p;
}

static long checked, failures;

/* One plane block with `n` bytes per lane: random lanes, or position-dependent
 * ramps when `ramp` is set. The destination starts `dst_off` bytes into its
 * allocation and ends exactly where the allocation ends. */
static void run_case(int stride, size_t n, size_t dst_off, int ramp)
{
    const size_t isize = n * (size_t)stride;
    const size_t srcSize = 5 + (size_t)stride * (1 + n);
    uint8_t* src = (uint8_t*)xmalloc(srcSize);
    uint8_t* ref = (uint8_t*)xmalloc(isize);
    uint8_t* buf = (uint8_t*)xmalloc(dst_off + isize);
    uint8_t* dst = buf + dst_off;

    src[0] = (uint8_t)(stride == 4 ? 0x0a : 0x08);     /* plane block, stride code 0 = 2, 2 = 4 */
    src[1] = (uint8_t)isize;
    src[2] = (uint8_t)(isize >> 8);
    src[3] = (uint8_t)(isize >> 16);
    src[4] = (uint8_t)(isize >> 24);
    uint8_t* q = src + 5;
    for (int lane = 0; lane < stride; lane++) {
        *q++ = 0;                                      /* raw lane */
        for (size_t j = 0; j < n; j++) {
            const uint8_t v = ramp ? (uint8_t)(j * 7 + (size_t)lane * 61 + 1) : (uint8_t)xrand();
            q[j] = v;
            ref[j * (size_t)stride + (size_t)lane] = v;
        }
        q += n;
    }

    const size_t got = LZ6_decompress_seq((const char*)src, srcSize, (char*)dst, isize);
    checked++;
    if (got != isize || (isize && memcmp(dst, ref, isize) != 0)) {
        failures++;
        fprintf(stderr, "FAIL stride=%d n=%zu dst_off=%zu ramp=%d: returned %zu, expected %zu%s\n",
                stride, n, dst_off, ramp, got, isize, got == isize ? " (bytes differ)" : "");
    }
    if (isize > 0) {
        if (LZ6_decompress_seq((const char*)src, srcSize, (char*)dst, isize - 1) != 0) {
            failures++;
            fprintf(stderr, "FAIL stride=%d n=%zu: accepted a destination one byte short\n", stride, n);
        }
        uint8_t* s2 = (uint8_t*)xmalloc(srcSize - 1);
        memcpy(s2, src, srcSize - 1);
        if (LZ6_decompress_seq((const char*)s2, srcSize - 1, (char*)dst, isize) != 0) {
            failures++;
            fprintf(stderr, "FAIL stride=%d n=%zu: accepted a source one byte short\n", stride, n);
        }
        free(s2);
    }
    free(src); free(ref); free(buf);
}

int main(int argc, char** argv)
{
    const int iters = argc > 1 ? atoi(argv[1]) : 3000;
    for (int stride = 2; stride <= 4; stride += 2) {
        for (size_t n = 0; n <= 300; n++) {
            run_case(stride, n, 0, 0);
            run_case(stride, n, 1 + n % 15, 1);
        }
        for (int i = 0; i < iters; i++) {
            const size_t n = (xrand() & 7) == 0 ? (size_t)(xrand() % 200000) : (size_t)(xrand() % 5000);
            run_case(stride, n, xrand() & 15, i & 1);
        }
    }
    printf("%s: %ld plane blocks checked, %ld failures\n", failures ? "FAIL" : "PASS", checked, failures);
    return failures ? 1 : 0;
}
