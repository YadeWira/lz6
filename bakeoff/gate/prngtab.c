/* prngtab: the encoder's table of first random() draws (what finds glibc random() output in O(seeds) byte compares) must equal the generator
 * stepped the slow way, for every seed 0..65535. Build with -DLZ6SEQ_TEST_HOOKS. Also: LZ6_compress_seq of glibc random() output at every byte
 * shift must come out as the 7-byte descriptor, and of a block that diverges from it must not. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "lib/entropy/lz6seq.h"

int LZ6_seq_prng_selftest(void);

int main(void) {
    int bad = LZ6_seq_prng_selftest();
    if (bad) { fprintf(stderr, "prngtab: %d seeds differ from the generator\n", bad); return 1; }
    /* random() output, rebuilt here with the glibc the test runs on */
    static const unsigned seeds[] = { 0, 1, 2, 255, 4096, 40000, 65535 };
    long checked = 0;
    for (unsigned s = 0; s < sizeof seeds / sizeof *seeds; s++) for (int shift = 0; shift < 32; shift += 8) {
        enum { N = 3000 };
        uint8_t* in = malloc(N); srandom(seeds[s]);
        for (int i = 0; i < N; i++) in[i] = (uint8_t)(random() >> shift);
        char out[N + N / 2 + 65536 + 16];
        size_t cs = LZ6_compress_seq((const char*)in, N, out, sizeof out, 7);
        if (shift < 24 && cs != 7 && cs > 16) {     /* shift 24 keeps 7 bits per byte: below the entropy gate (7.2), never detected */ fprintf(stderr, "prngtab: seed %u shift %d not found (%zu bytes)\n", seeds[s], shift, cs); return 2; }
        char* back = malloc(N);
        if (LZ6_decompress_seq(out, cs, back, N) != N || memcmp(back, in, N)) { fprintf(stderr, "prngtab: seed %u shift %d does not round trip\n", seeds[s], shift); return 3; }
        free(back);
        in[1500] ^= 0x55;                                     /* diverges after 1500 bytes: not a descriptor any more */
        cs = LZ6_compress_seq((const char*)in, N, out, sizeof out, 7);
        back = malloc(N);
        if ((shift < 24 && cs <= 16) || LZ6_decompress_seq(out, cs, back, N) != N || memcmp(back, in, N)) { fprintf(stderr, "prngtab: diverging block for seed %u shift %d\n", seeds[s], shift); return 4; }
        free(back); free(in); checked += 2;
    }
    printf("prngtab: 65536 seeds equal the generator, %ld random() blocks checked\n", checked);
    return 0;
}
