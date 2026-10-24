/* lowaddr: the compressed bytes must not depend on where the source buffer sits in memory. The match finder keeps a pointer `base` = start - 2^windowLog
 * (the window of the chain and tree strategies, up to 32 MB), and five 3-byte-match checks compared a match pointer with it (`match > base`): when the
 * buffer was below 2^windowLog in memory (a static non-PIE binary with a small heap randomization, 32-bit builds, no ASLR) `base` wrapped to a huge value,
 * the checks failed, the 3-byte matches were never taken and the output changed: v1.6.15-pre gave two different streams for the same file and options, the
 * one in 1% of the runs, and the second more often on a system without ASLR. A 20 MB input is compressed with the buffer at 0x800000 (8 MB, under both the
 * 16 MB and the 32 MB windows) and in malloc'd memory; at every level the output must be the same and must decode. Build WITHOUT sanitizers (they claim low
 * addresses); skipped, with a note, when the address cannot be mapped. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include "lib/entropy/lz6seq.h"
#include "lz6.h"
#include "lz6hc.h"

static uint64_t st = 0x9E3779B97F4A7C15ull;
static uint8_t rb(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return (uint8_t)(st >> 24); }

static size_t compress(int hc, int level, const uint8_t* src, size_t n, char* dst, size_t cap) {
    if (hc) { int r = LZ6_compress_HC((const char*)src, dst, (int)n, (int)cap, level); return r > 0 ? (size_t)r : 0; }
    return LZ6_compress_seq((const char*)src, n, dst, cap, level);
}

int main(void) {
    const size_t N = (size_t)20 << 20;
    uint8_t* hi = malloc(N);
    /* text-like data with repeats at short and long distances, so that 3-byte matches matter */
    static const char* w[] = { "the ", "of ", "and ", "lz6 ", "block ", "match ", "stable ", "offset ", "\n", "xy", "q" };
    for (size_t i = 0; i < N;) { if (rb() % 9 == 0) { size_t l = rb() % 24; while (l-- && i < N) hi[i++] = rb(); } else { const char* s = w[rb() % (sizeof w / sizeof *w)]; while (*s && i < N) hi[i++] = (uint8_t)*s++; } }
    uint8_t* lo = mmap((void*)0x800000, N + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (lo == MAP_FAILED || (uintptr_t)lo != 0x800000) { printf("lowaddr: skipped (cannot map 0x800000)\n"); return 0; }
    memcpy(lo, hi, N);
    size_t cap = N + N / 2 + 65536; char* a = malloc(cap); char* b = malloc(cap); uint8_t* d = malloc(N);
    struct { int hc, level; } cfg[] = { {0,-2}, {0,-1}, {0,1}, {0,3}, {0,5}, {0,8}, {1,3}, {1,9} };
    int bad = 0, n = 0;
    for (unsigned i = 0; i < sizeof cfg / sizeof *cfg; i++) {
        size_t ca = compress(cfg[i].hc, cfg[i].level, hi, N, a, cap), cb = compress(cfg[i].hc, cfg[i].level, lo, N, b, cap);
        int same = ca && ca == cb && !memcmp(a, b, ca);
        size_t r = 0;
        if (!cfg[i].hc && cb) r = LZ6_decompress_seq(b, cb, (char*)d, N);
        else if (cb) { int x = LZ6_decompress_safe(b, (char*)d, (int)cb, (int)N); r = x > 0 ? (size_t)x : 0; }
        int ok = cb && r == N && !memcmp(d, hi, N);
        if (!same || (!cfg[i].hc && !ok)) { bad = 1; fprintf(stderr, "lowaddr FAIL: %s level %d: malloc %zu bytes, 0x800000 %zu bytes, %s\n", cfg[i].hc ? "HC" : "seq", cfg[i].level, ca, cb, same ? "decode failed" : "outputs differ"); }
        n++;
    }
    printf(bad ? "lowaddr: FAILED\n" : "lowaddr: %d levels give the same bytes at 0x800000 and in malloc'd memory\n", n);
    return bad;
}
