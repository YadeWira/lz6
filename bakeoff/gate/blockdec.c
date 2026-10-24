/* blockdec [N]: the plain block decoder (LZ6_decompress_safe and LZ6_decompress_safe_partial) on hostile input. Input and output are
 * EXACT-size heap blocks, so ASAN sees a read past the end of the compressed data as well as a write past the end of the output.
 * Build with -fsanitize=address,undefined. It feeds:
 *   1) the empty input and every 1-byte and 2-byte input, at several output capacities (a block cut right after its token used to be
 *      read one byte past its end: the first extension byte of a long literal run was not bounds-checked, and neither was an empty input);
 *   2) valid blocks made by LZ6_compress_default and LZ6_compress_HC at levels 1-15, then N mutants of them: bit flips, truncations,
 *      random runs, inserted bytes.
 * A safe decoder must never read or write out of bounds, and must never report more bytes than the capacity it was given. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "lz6.h"
#include "lz6hc.h"

static uint64_t st = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; }
static long cases, bad;

static void dec(const uint8_t* b, size_t n, int cap, int partial) {
    uint8_t* e = malloc(n);                         /* exact size: malloc(0) is fine, ASAN guards it */
    if (n) memcpy(e, b, n);
    uint8_t* d = malloc(cap > 0 ? (size_t)cap : 1);
    int r = partial ? LZ6_decompress_safe_partial((const char*)e, (char*)d, (int)n, cap / 2, cap)
                    : LZ6_decompress_safe((const char*)e, (char*)d, (int)n, cap);
    if (r > cap) { bad++; fprintf(stderr, "BAD: %d bytes into a capacity of %d (input of %zu)\n", r, cap, n); }
    cases++;
    free(d); free(e);
}

static size_t make_text(uint8_t* o, size_t n) {     /* word-salad text and some structured binary: compressible */
    static const char* w[] = { "the ", "of ", "lz6 ", "block ", "decoder ", "offset ", "literal ", "match ", "stable ", "ratio ", "\n", "0123456789", "ABCDEF" };
    size_t i = 0;
    while (i < n) {
        if (rnd() % 7 == 0) { size_t l = rnd() % 40; while (l-- && i < n) o[i++] = (uint8_t)rnd(); }
        else { const char* s = w[rnd() % (sizeof w / sizeof *w)]; while (*s && i < n) o[i++] = (uint8_t)*s++; }
    }
    return n;
}

int main(int argc, char** argv) {
    long N = argc > 1 ? atol(argv[1]) : 100000;
    static const int caps[] = { 0, 1, 7, 64, 304, 100000 };
    for (unsigned c = 0; c < sizeof caps / sizeof *caps; c++) for (int p = 0; p < 2; p++) {
        dec((const uint8_t*)"", 0, caps[c], p);
        for (unsigned a = 0; a < 256; a++) {
            uint8_t b[2] = { (uint8_t)a, 0 };
            dec(b, 1, caps[c], p);
            for (unsigned x = 0; x < 256; x += (c < 4 ? 1 : 17)) { b[1] = (uint8_t)x; dec(b, 2, caps[c], p); }
        }
    }
    enum { SZ = 40000 };
    uint8_t* src = malloc(SZ); make_text(src, SZ);
    int bound = LZ6_compressBound(SZ);
    uint8_t* blk[16]; int blen[16]; int nb = 0;
    for (int lv = 0; lv <= 15; lv++) {
        uint8_t* c = malloc(bound);
        int cs = lv ? LZ6_compress_HC((const char*)src, (char*)c, SZ, bound, lv) : LZ6_compress_default((const char*)src, (char*)c, SZ, bound);
        if (cs <= 0) { fprintf(stderr, "compress failed at level %d\n", lv); return 3; }
        uint8_t* d = malloc(SZ);
        if (LZ6_decompress_safe((const char*)c, (char*)d, cs, SZ) != SZ || memcmp(d, src, SZ)) { fprintf(stderr, "round trip failed at level %d\n", lv); return 4; }
        free(d); blk[nb] = c; blen[nb++] = cs;
    }
    for (long it = 0; it < N; it++) {
        int k = (int)(rnd() % nb); size_t n = (size_t)blen[k];
        uint8_t* m = malloc(n + 16); memcpy(m, blk[k], n);
        int kind = (int)(rnd() % 5);
        if (kind == 0) { int f = 1 + (int)(rnd() % 8); while (f--) m[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8)); }
        else if (kind == 1) n = rnd() % n;
        else if (kind == 2) { size_t p = rnd() % n, l = 1 + rnd() % 64; for (size_t j = p; j < p + l && j < n; j++) m[j] = (uint8_t)rnd(); }
        else if (kind == 3) { size_t p = rnd() % n; memmove(m + p + 1, m + p, n - p); m[p] = (uint8_t)rnd(); n++; }
        else { size_t p = n > 8 ? n - 1 - rnd() % 8 : 0; m[p] = (uint8_t)rnd(); }   /* the tail: where the last token and its literals sit */
        int cap = (rnd() % 3 == 0) ? (int)(rnd() % (SZ + 64)) : (rnd() % 2 ? SZ : SZ + (int)(rnd() % 64));
        dec(m, n, cap, (int)(rnd() % 4 == 0));
        free(m);
    }
    for (int i = 0; i < nb; i++) free(blk[i]);
    free(src);
    printf("blockdec: %ld cases, %ld bad, no crash\n", cases, bad);
    return bad ? 1 : 0;
}
