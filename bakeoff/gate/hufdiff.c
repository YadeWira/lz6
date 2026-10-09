/* hufdiff N : literal-Huffman differential fuzz. Builds 4-stream Huffman blocks (huf_build_header + huf_encode4_8) over
 * skewed random data, then decodes valid and MUTATED blocks (header lengths, stream bytes, truncation) through
 * huf_dec_tables + huf_decode4 from EXACT-size heap buffers. Prints "case rc hash" per case: build the program against two
 * trees and diff the outputs (the fast loop must be byte- and rc-identical to the old one, also on corrupt input). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "lib/entropy/lz6huf.h"
static uint64_t st = 0x2545F4914F6CDD1Dull;
static uint64_t rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; }
int main(int argc, char** argv) {
    long N = atol(argv[1]); long valid_ok = 0;
    for (long it = 0; it < N; it++) {
        size_t n = (size_t)(rnd() % 6 == 0 ? rnd() % 64 : rnd() % 6 == 0 ? rnd() % 200000 : 1 + rnd() % 6000);
        int alpha = 2 + (int)(rnd() % 255); double skew = 0.3 + (rnd() % 200) / 40.0;
        unsigned cnt[256]; memset(cnt, 0, sizeof cnt);
        uint8_t* s = malloc(n ? n : 1);
        double w[256], tot = 0; for (int i = 0; i < alpha; i++) { double x = 1.0; for (int j = 0; j < (int)(skew * (i % 17)); j++) x *= 0.7; w[i] = x + 1e-4; tot += w[i]; }
        for (size_t i = 0; i < n; i++) { double r = (rnd() % 1000003) / 1000003.0 * tot; int c = 0; while (c < alpha - 1 && r > w[c]) { r -= w[c]; c++; } s[i] = (uint8_t)c; cnt[c]++; }
        uint8_t hdr[300]; int k = 0, lim = 0;
        int maxbits = (rnd() & 1) ? 12 : 16;
        size_t hh = huf_build_header(cnt, 255, hdr, sizeof hdr, &k, &lim, maxbits);
        size_t cap = n + n / 2 + 4096; uint8_t* enc = malloc(cap);
        size_t es = hh ? huf_encode4_8(hdr, s, n, enc, cap) : 0;
        if (!hh || !es) { free(s); free(enc); printf("%ld skip\n", it); continue; }
        size_t tot_len = hh + es; uint8_t* b = malloc(tot_len); memcpy(b, hdr, hh); memcpy(b + hh, enc, es);
        int kind = (int)(rnd() % 6);
        size_t len = tot_len;
        if (kind == 1) { int m = 1 + (int)(rnd() % 6); for (int j = 0; j < m; j++) b[rnd() % hh] ^= (uint8_t)(1u << (rnd() % 8)); }          /* header lengths */
        else if (kind == 2) { int m = 1 + (int)(rnd() % 6); for (int j = 0; j < m; j++) b[hh + rnd() % es] ^= (uint8_t)(1u << (rnd() % 8)); } /* stream */
        else if (kind == 3) { len = hh + rnd() % (es + 1); }                                                                                  /* truncated */
        else if (kind == 4) { size_t p = rnd() % tot_len, l = 1 + rnd() % 8; for (size_t j = p; j < p + l && j < tot_len; j++) b[j] = (uint8_t)rnd(); }
        else if (kind == 5) { b[hh + (rnd() % 4 == 0 ? 0 : rnd() % (es < 4 ? es : 4))] ^= (uint8_t)rnd(); }                                   /* stream sizes */
        uint8_t* e = malloc(len ? len : 1); memcpy(e, b, len);
        huf_dstate d; memset(&d, 0, sizeof d);
        int rc = -1; uint64_t h = 1469598103934665603ull;
        uint8_t* o = malloc(n ? n : 1);
        if (len >= hh && huf_dec_tables(&d, e, hh)) {
            size_t r = huf_decode4(&d, e + hh, len - hh, o, n);
            rc = (int)(r != 0);
            if (r) for (size_t q = 0; q < n; q++) { h ^= o[q]; h *= 1099511628211ull; }
            if (kind == 0 && n && (r == 0 || memcmp(o, s, n))) { fprintf(stderr, "VALID BLOCK DECODE MISMATCH it=%ld n=%zu k=%d\n", it, n, k); return 4; }
            if (kind == 0) valid_ok++;
            huf_dec_free(&d);
        }
        printf("%ld %d %016llx\n", it, rc, (unsigned long long)h);
        free(o); free(e); free(b); free(enc); free(s);
    }
    fprintf(stderr, "hufdiff: %ld cases, %ld valid ok\n", N, valid_ok);
    return 0;
}
