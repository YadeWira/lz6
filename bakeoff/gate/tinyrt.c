/* tinyrt N FILE...: round trip many TINY inputs (8..700 bytes: slices of real files and synthetic runs) through the seq codec at several levels,
 * source and destination as exact-size heap copies (ASAN sees any over-read/over-write). Reports how many compressed (seq) blocks were exercised. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "lib/entropy/lz6seq.h"
static uint64_t st = 0x2545F4914F6CDD1Dull; static uint64_t rnd(void){ st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; }
int main(int argc, char** argv)
{
    long N = atol(argv[1]); int nf = argc - 2; uint8_t* data[16]; long dsz[16];
    for (int i = 0; i < nf; i++) { FILE* f = fopen(argv[2 + i], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); if (n > 4000000) n = 4000000; fseek(f, 0, SEEK_SET); data[i] = malloc(n); if (fread(data[i], 1, n, f) != (size_t)n) return 1; fclose(f); dsz[i] = n; }
    static const int lv[] = { 1, 2, 3, 5, 8, 12, 15 };
    long blocks = 0, total = 0, bad = 0;
    for (long it = 0; it < N; it++) {
        size_t n = 8 + rnd() % 700; uint8_t* src = malloc(n);
        int kind = rnd() % (nf + 3);
        if (kind < nf) { long p = rnd() % (dsz[kind] - n); memcpy(src, data[kind] + p, n); }
        else if (kind == nf) { memset(src, 'a' + rnd() % 3, n); }
        else { size_t per = 1 + rnd() % 9; for (size_t i = 0; i < n; i++) src[i] = (uint8_t)("abcdefghijk"[(i % per) + (i / (per * 7)) % 2]); }
        for (int li = 0; li < 7; li++) {
            size_t cap = n + n / 2 + 4096; uint8_t* c = malloc(cap);
            size_t cs = LZ6_compress_seq((const char*)src, n, (char*)c, cap, lv[li]);
            total++;
            if (!cs) { free(c); continue; }
            if (cs < n) blocks++;
            uint8_t* e = malloc(cs); memcpy(e, c, cs); uint8_t* d = malloc(n);
            size_t r = LZ6_decompress_seq((const char*)e, cs, (char*)d, n);
            if (r != n || memcmp(d, src, n)) { bad++; if (bad < 5) fprintf(stderr, "MISMATCH it=%ld level=%d n=%zu cs=%zu r=%zu\n", it, lv[li], n, cs, r); }
            free(e); free(d); free(c);
        }
        free(src);
    }
    for (int i = 0; i < nf; i++) free(data[i]);
    printf("tinyrt: %ld round trips, %ld compressed blocks, %ld BAD\n", total, blocks, bad);
    return bad != 0;
}
