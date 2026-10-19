/* guard.c - regression test for the level guard of the seq encoder (seq_guard in lib/entropy/lz6seq.c).
 *
 * Data: independent symbols from a skewed 21-letter alphabet (about 4.17 bits each, the statistics of a protein
 * file). Matches on it save little: a 3-byte match saves 12 bits and costs about 16. Before the guard the optimal
 * and the row parsers (rows 4-15) took thousands of them and the output was 2.5-6% LARGER than the fast tier's fastest
 * (a protein file of the AIT set: rows 4-15 were 4-5% larger than row 1, and 6% above zstd -19).
 * Checks, for levels -1 and 1..13 (rows 2..15): the stream decodes (exact-size source and destination), and levels 2..13 (rows
 * 4..15) are at most 2% larger than level -2 (row 1: the unguarded encoder gives +2.5% at row 8, +3.1% at row 4, +6.2% at row 7).
 * Build with ASAN/UBSan like the other gate tests:
 *   cc -O1 -g -fsanitize=address,undefined -I. -Ilib -Ilib/entropy bakeoff/gate/guard.c lib/lz6.c lib/lz6hc.c \
 *      lib/entropy/lz6seq.c lib/entropy/lz6fse.c lib/entropy/lz6rc.c lib/entropy/lz6huf.c -lm -o guard && ./guard */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "lib/entropy/lz6seq.h"

static uint64_t st = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { st ^= st >> 12; st ^= st << 25; st ^= st >> 27; return (uint32_t)((st * 0x2545F4914F6CDD1Dull) >> 32); }

static size_t compress_rt(const unsigned char* src, size_t n, int level, int* ok)
{
    char* dst = (char*)malloc(n + n / 2 + 70000);
    const size_t cs = LZ6_compress_seq((const char*)src, n, dst, n + n / 2 + 70000, level);
    *ok = 0;
    if (cs) {
        char* e = (char*)malloc(cs); memcpy(e, dst, cs);
        char* d = (char*)malloc(n);
        const size_t r = LZ6_decompress_seq(e, cs, d, n);
        *ok = r == n && memcmp(d, src, n) == 0;
        free(d); free(e);
    }
    free(dst);
    return cs;
}

int main(void)
{
    static const unsigned w[21] = { 139621, 125393, 96649, 92844, 78544, 75681, 75485, 72146, 70316, 67179, 58072,
                                    58038, 50000, 45000, 40000, 35000, 30000, 25000, 20000, 15000, 12000 };
    unsigned tot = 0, cum[21];
    for (int i = 0; i < 21; i++) { tot += w[i]; cum[i] = tot; }
    const size_t n = 300000;
    unsigned char* s = (unsigned char*)malloc(n);
    for (size_t i = 0; i < n; i++) { const unsigned r = rnd() % tot; int c = 0; while (cum[c] <= r) c++; s[i] = (unsigned char)('A' + c); }
    LZ6_seq_init();
    int ok1;
    const size_t base = compress_rt(s, n, -2, &ok1);
    int bad = !ok1 || !base;
    printf("level -2: %zu bytes%s\n", base, ok1 ? "" : "  ROUND TRIP FAILED");
    for (int level = -1; level <= 13; level++) {
        if (level == 0) continue;
        int ok;
        const size_t cs = compress_rt(s, n, level, &ok);
        const int big = level >= 2 && (double)cs > 1.02 * (double)base;
        printf("level %2d: %zu bytes (%+.2f%% vs level -2)%s%s\n", level, cs, 100.0 * ((double)cs / (double)base - 1),
               ok ? "" : "  ROUND TRIP FAILED", big ? "  TOO LARGE" : "");
        bad |= !ok || !cs || big;
    }
    free(s);
    puts(bad ? "FAIL: level guard" : "PASS: level guard");
    return bad ? 1 : 0;
}
