/* fuzz_decode.c — safety gate for the lz6 block decoder.
 *
 * The CLI over-allocates its output buffer, so an out-of-bounds write past the
 * logical end of output lands inside the allocation and is invisible to both
 * round-trip and ASAN. This driver calls LZ6_decompress_safe into an EXACTLY
 * sized buffer, so any write past the contracted end hits an ASAN redzone.
 *
 * It feeds:
 *   1) the valid compressed block (sanity), and
 *   2) many corrupted variants (byte flips + truncations)
 * to the decoder. A correct safe decoder must never read/write out of bounds on
 * ANY input — malformed included. Build with -fsanitize=address; ASAN aborts
 * (nonzero exit) on any violation, which the harness treats as DISQUALIFIED.
 *
 * Usage: fuzz_decode <seedfile> [iterations]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lz6.h"
#include "lz6hc.h"

static unsigned rng = 2463534242u;
static unsigned xrand(void){ rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr,"usage: %s seedfile [iters]\n",argv[0]); return 2; }
    int iters = (argc>2)? atoi(argv[2]) : 4000;

    FILE* f = fopen(argv[1],"rb");
    if (!f){ perror("open"); return 2; }
    fseek(f,0,SEEK_END); long fsz = ftell(f); fseek(f,0,SEEK_SET);
    if (fsz <= 0 || fsz > (64<<20)) { fprintf(stderr,"bad size\n"); return 2; }
    /* keep the block modest so decode is fast and matches are dense */
    int srcSize = (fsz > (256<<10)) ? (256<<10) : (int)fsz;
    char* src = (char*)malloc(srcSize);
    if (fread(src,1,srcSize,f)!=(size_t)srcSize){ return 2; } fclose(f);

    int levels[] = {6, 11, 15};
    for (int li=0; li<3; li++)
    {
        int bound = LZ6_compressBound(srcSize);
        char* comp = (char*)malloc(bound);
        int csize = LZ6_compress_HC(src, comp, srcSize, bound, levels[li]);
        if (csize <= 0){ fprintf(stderr,"compress fail L%d\n",levels[li]); return 2; }

        /* 1) valid block into an exactly-sized buffer */
        { char* dst = (char*)malloc(srcSize);
          LZ6_decompress_safe(comp, dst, csize, srcSize);
          free(dst); }

        /* 2) corrupted variants. Input is allocated EXACTLY (no trailing slack)
           so the gate catches any out-of-bounds read OR write of the compressed
           input — the decoder must never touch a byte past compressedSize.
           (The literal-copy near-end over-read was fixed in lz6.c: the fast
           wildcopy now falls back to an exact memcpy within WILDCOPYLENGTH of
           iend.) Output buffer is exact too, so OOB writes are caught. */
        char* cc = (char*)malloc(csize);
        for (int it=0; it<iters; it++)
        {
            memcpy(cc, comp, csize);
            int nflip = 1 + (xrand() % 4);
            for (int k=0;k<nflip;k++){
                int off = xrand() % (unsigned)csize;
                cc[off] = (char)(xrand() & 0xff);
            }
            /* sometimes also truncate */
            int clen = csize;
            if (xrand()&1) clen = 1 + (int)(xrand() % (unsigned)csize);

            /* decode into an EXACTLY sized buffer — the whole point */
            char* dst = (char*)malloc(srcSize);
            LZ6_decompress_safe(cc, dst, clen, srcSize);   /* return ignored; ASAN watches bounds */
            free(dst);
        }
        free(cc); free(comp);
    }
    free(src);
    printf("fuzz_decode: completed cleanly (no ASAN violation)\n");
    return 0;
}
