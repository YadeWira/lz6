/* seqbig: blocks of real size that the block format's limits used to break (a literal run or a match of more than 2^24 bytes: the stream did not
 * decode). Each case is compressed with LZ6_compress_seq at the given levels, decoded into an exact-size buffer and compared. Build with ASAN+UBSan.
 *   1) 1 MB of text-like data, then 17.5 MB of pseudo-random bytes (a compressible start in front of a compressed region: one literal run of 16.3 MiB+)
 *   2) 17.5 MB of pseudo-random bytes after 1 MB of zeros
 *   3) 40 MB of zeros (a match of 40 MB)
 *   4) random, zeros, random: a run over the limit, then a match over it, then another run */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "lib/entropy/lz6seq.h"

static uint64_t st = 0x9E3779B97F4A7C15ull;
static uint8_t rb(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return (uint8_t)(st >> 24); }
static void rnd(uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = rb(); }
static void text(uint8_t* p, size_t n) {
    static const char* w[] = { "the ", "block ", "decoder ", "literal ", "match ", "offset ", "stable ", "lz6 ", "ratio ", "\n" };
    size_t i = 0;
    while (i < n) { const char* s = w[rb() % (sizeof w / sizeof *w)]; while (*s && i < n) p[i++] = (uint8_t)*s++; }
}

static int run(const char* name, const uint8_t* src, size_t n, const int* lv, int nl) {
    size_t cap = n + n / 2 + 65536; char* c = malloc(cap); uint8_t* d = malloc(n);
    int bad = 0;
    for (int i = 0; i < nl; i++) {
        size_t cs = LZ6_compress_seq((const char*)src, n, c, cap, lv[i]);
        size_t r = cs ? LZ6_decompress_seq(c, cs, (char*)d, n) : 0;
        if (!cs || r != n || memcmp(d, src, n)) { fprintf(stderr, "seqbig FAIL: %s level %d (%zu -> %zu, decoded %zu)\n", name, lv[i], n, cs, r); bad = 1; }
        else printf("seqbig ok: %-28s level %-3d %9zu -> %9zu\n", name, lv[i], n, cs);
    }
    free(c); free(d);
    return bad;
}

int main(void) {
    static const int lv[] = { -2, -1, 3, 5 }, lvz[] = { -2, 5 };
    int bad = 0; size_t M = (size_t)1 << 20;
    uint8_t* b = malloc(41 * M);
    text(b, M); rnd(b + M, 17 * M + M / 2);                 bad |= run("text + 17.5 MB random", b, 18 * M + M / 2, lv, 4);
    memset(b, 0, M); rnd(b + M, 17 * M + M / 2);            bad |= run("zeros + 17.5 MB random", b, 18 * M + M / 2, lv, 4);
    memset(b, 0, 40 * M);                                   bad |= run("40 MB zeros", b, 40 * M, lvz, 2);
    rnd(b, 17 * M + M / 2); memset(b + 17 * M + M / 2, 0, 20 * M); rnd(b + 37 * M + M / 2, 3 * M);
    bad |= run("random, 20 MB zeros, random", b, 40 * M + M / 2 - M / 2, lvz, 2);
    free(b);
    printf(bad ? "seqbig: FAILED\n" : "seqbig: all cases round trip\n");
    return bad;
}
