/* planedelta.c - byte-plane blocks whose lanes are coded as byte deltas (mode bit 7): round trips, the size win, and a mutation fuzz.
 *
 * Smooth 16-bit samples (a random walk, noisy low byte) and smooth 32-bit values go through LZ6_compress_seq at levels 2, 3, 6 and 13
 * (delta lanes are tried from level 2 up) three ways: without deltas (LZ6_NO_PLANEDELTA), as the encoder chooses them, and with the delta mode
 * forced on every lane (LZ6_PLANEDELTA_FORCE, a test hook: real lanes take it only when it saves 3%). All three must round trip from an exact-size
 * source into an exact-size destination, the forced stream must carry the delta bit on every lane (the walk of the lane headers checks it) and the
 * normal one must use deltas on at least one lane and be at least 20% smaller than the one without (a random walk: it is 40-60%). Then 4000 mutants of each forced stream (bit flips, byte runs, truncation) are decoded
 * from exact-size copies: no crash, no ASAN report.
 * Build with ASAN/UBSan like the other gate tests:
 *   cc -O1 -g -fsanitize=address,undefined -I. -Ilib -Ilib/entropy bakeoff/gate/planedelta.c lib/lz6.c lib/lz6hc.c \
 *      lib/entropy/lz6seq.c lib/entropy/lz6fse.c lib/entropy/lz6rc.c lib/entropy/lz6huf.c -lm -o planedelta && ./planedelta */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "lib/entropy/lz6seq.h"
static uint64_t st = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(void) { st ^= st >> 12; st ^= st << 25; st ^= st >> 27; return (uint32_t)((st * 0x2545F4914F6CDD1Dull) >> 32); }
static size_t comp(const uint8_t* s, size_t n, int level, int mode, uint8_t** out)   /* mode 0: no deltas, 1: normal, 2: deltas forced on every lane */
{
    const size_t cap = n + n / 2 + 70000; uint8_t* c = (uint8_t*)malloc(cap);
    unsetenv("LZ6_NO_PLANEDELTA"); unsetenv("LZ6_PLANEDELTA_FORCE");
    if (mode == 0) setenv("LZ6_NO_PLANEDELTA", "1", 1);
    if (mode == 2) setenv("LZ6_PLANEDELTA_FORCE", "1", 1);
    const size_t cs = LZ6_compress_seq((const char*)s, n, (char*)c, cap, level);
    unsetenv("LZ6_NO_PLANEDELTA"); unsetenv("LZ6_PLANEDELTA_FORCE");
    *out = c; return cs;
}
/* walks a plane block's lanes and counts those with the delta bit (mode bit 7); -1 when it is not a plane block */
static int delta_lanes(const uint8_t* c, size_t cs)
{
    if (cs < 6 || !(c[0] & 0x08)) return -1;
    const int stride = (c[0] & 3) == 2 ? 4 : 2; uint32_t isize; memcpy(&isize, c + 1, 4);
    const size_t plane = isize / (size_t)stride; size_t q = 5; int n = 0;
    for (int l = 0; l < stride; l++) {
        if (q >= cs) return -1;
        const int m = c[q++]; n += (m & 0x80) != 0;
        if ((m & 0x7f) == 0) q += plane;
        else { uint32_t z; memcpy(&z, c + q, 4); q += 4 + (size_t)z; }
    }
    return n;
}
static int rt(const uint8_t* s, size_t n, const uint8_t* c, size_t cs)
{
    uint8_t* e = (uint8_t*)malloc(cs); memcpy(e, c, cs); uint8_t* d = (uint8_t*)malloc(n);
    const size_t r = LZ6_decompress_seq((const char*)e, cs, (char*)d, n);
    const int ok = r == n && memcmp(d, s, n) == 0;
    free(e); free(d); return ok;
}
int main(void)
{
    LZ6_seq_init();
    int bad = 0; long mutants = 0;
    for (int kind = 0; kind < 2; kind++) {              /* 0: 16-bit samples, 1: 32-bit values */
        const size_t n = kind ? 400000 : 400002; uint8_t* s = (uint8_t*)malloc(n);
        if (kind == 0) { int v = 30000; for (size_t i = 0; i + 1 < n; i += 2) { v += (int)(rnd() % 9) - 4; s[i] = (uint8_t)(v & 0xff); s[i + 1] = (uint8_t)(v >> 8); } }
        else { int64_t v = 1 << 20; for (size_t i = 0; i + 3 < n; i += 4) { v += (int)(rnd() % 7) - 3; memcpy(s + i, &v, 4); } }
        static const int lv[] = { 2, 3, 6, 13 };
        for (unsigned li = 0; li < 4; li++) {
            uint8_t *c0, *c1, *c2;
            const size_t a = comp(s, n, lv[li], 0, &c0), b = comp(s, n, lv[li], 1, &c1), f = comp(s, n, lv[li], 2, &c2);
            const int ok = a && b && f && rt(s, n, c0, a) && rt(s, n, c1, b) && rt(s, n, c2, f);
            const int forced = delta_lanes(c2, f), normal = delta_lanes(c1, b);
            printf("%s level %2d: no deltas %zu, normal %zu (%d delta lanes), forced %zu (%d delta lanes)%s%s\n", kind ? "32-bit" : "16-bit", lv[li], a, b, normal, f, forced,
                   ok ? "" : "  ROUND TRIP FAILED", forced == (kind ? 4 : 2) && normal >= 1 ? "" : "  DELTA LANES MISSING");
            bad |= !ok || forced != (kind ? 4 : 2) || b > a || normal < 1 || (double)b > 0.8 * (double)a;   /* smooth data: the encoder takes the deltas, and wins big */
            if (f && ok) {                                       /* mutate the forced stream: every lane is delta-coded */
                for (int m = 0; m < 4000; m++) {
                    size_t len = f; uint8_t* e = (uint8_t*)malloc(f); memcpy(e, c2, f);
                    switch (m % 4) {
                    case 0: for (int j = 0; j < 1 + (int)(rnd() % 4); j++) e[rnd() % f] ^= (uint8_t)(1u << (rnd() % 8)); break;
                    case 1: { size_t p = rnd() % (f < 64 ? f : 64); for (size_t j = p; j < p + 6 && j < f; j++) e[j] = (uint8_t)rnd(); } break;   /* headers: modes, sizes */
                    case 2: len = 1 + rnd() % f; break;
                    default: e[5 + rnd() % 8] ^= (uint8_t)rnd(); break;
                    }
                    uint8_t* d = (uint8_t*)malloc(n);
                    (void)LZ6_decompress_seq((const char*)e, len, (char*)d, n);
                    free(d); free(e); mutants++;
                }
            }
            free(c0); free(c1); free(c2);
        }
        free(s);
    }
    printf("%ld mutants decoded\n", mutants);
    puts(bad ? "FAIL: plane delta" : "PASS: plane delta");
    return bad ? 1 : 0;
}
