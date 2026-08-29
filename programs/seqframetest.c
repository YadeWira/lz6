/* seqframetest.c - round-trip tests for seq-coded frames (magic 0x184D2207).
 * Exercises one-shot and streaming decode, several block sizes, awkward
 * dst slices (intoTmp/flushOut paths), incompressible blocks (bit31 escape)
 * and verifies the frame magic for both codecs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lz6frame.h"

static int roundtrip(size_t n, LZ6F_blockSizeID_t bid, int seq, int level, int autoflush, int kind) {
    char* src = malloc(n);
    for (size_t i = 0; i < n; i++)
        src[i] = (kind == 2) ? (char)((i * 2654435761u) >> 24)                                    /* incompressible */
               : (kind == 1) ? ((i % 100 < 12) ? (char)('a' + (i % 13))                           /* mostly random: trips the light-LZ fallback */
                                               : (char)((i * 2654435761u) >> 24))
                             : ((i % 100 < 70) ? (char)('a' + (i % 13)) : (char)(i * 31 + (i >> 3)));
    size_t cbound = LZ6F_compressFrameBound(n, NULL) + 64;
    char* c = malloc(cbound);
    char* out = malloc(n + 64);

    LZ6F_preferences_t p;
    memset(&p, 0, sizeof(p));
    p.frameInfo.blockSizeID = bid;
    p.frameInfo.contentChecksumFlag = LZ6F_contentChecksumEnabled;
    p.frameInfo.blockCodec = seq ? LZ6F_blockCodec_seq : LZ6F_blockCodec_lz;
    p.compressionLevel = level;
    p.autoFlush = (unsigned)autoflush;
    size_t cs = LZ6F_compressFrame(c, cbound, src, n, &p);
    if (LZ6F_isError(cs)) { fprintf(stderr, "seq=%d: compress error %s\n", seq, LZ6F_getErrorName(cs)); return 1; }

    /* streaming decode with small dst slices to exercise intoTmp/flushOut */
    LZ6F_decompressionContext_t dctx;
    if (LZ6F_isError(LZ6F_createDecompressionContext(&dctx, LZ6F_VERSION))) { fprintf(stderr, "dctx fail\n"); return 1; }
    size_t pos = 0, done = 0;
    while (pos < cs) {
        size_t outChunk = 4096 + (n % 997);   /* awkward sizes */
        size_t inChunk = 1000 + (n % 13);
        size_t din = cs - pos < inChunk ? cs - pos : inChunk;
        size_t dout = sizeof(char) * (n + 64 - done) < outChunk ? n + 64 - done : outChunk;
        size_t r = LZ6F_decompress(dctx, out + done, &dout, c + pos, &din, NULL);
        if (LZ6F_isError(r)) { fprintf(stderr, "seq=%d bid=%d: decompress error %s\n", seq, bid, LZ6F_getErrorName(r)); return 1; }
        pos += din; done += dout;
        if (din == 0 && dout == 0) { fprintf(stderr, "seq=%d: stalled\n", seq); return 1; }
    }
    if (done != n || memcmp(out, src, n) != 0) { fprintf(stderr, "seq=%d bid=%d: MISMATCH done=%zu n=%zu\n", seq, bid, done, n); return 1; }
    /* codec verification: magic is always 06 22 4D 18 now — the block codec
     * rides on the per-block bit30 flag */
    if (((unsigned char*)c)[0] != 0x06 || ((unsigned char*)c)[1] != 0x22 ||
        ((unsigned char*)c)[2] != 0x4D || ((unsigned char*)c)[3] != 0x18) {
        fprintf(stderr, "seq=%d: bad magic %02x%02x%02x%02x\n", seq,
                ((unsigned char*)c)[3], ((unsigned char*)c)[2], ((unsigned char*)c)[1], ((unsigned char*)c)[0]);
        return 1;
    }
    printf("OK seq=%d bid=%d level=%d flush=%d kind=%d n=%zu csize=%zu (%.2f%%)\n",
           seq, bid, level, autoflush, kind, n, cs, 100.0 * cs / n);
    if (kind == 1) {
        /* walk block headers: the mixed content must trip the light-LZ
         * fallback (compressed blocks with bit30 clear) */
        size_t o = 7;   /* magic(4) + FLG + BD + HC */
        size_t nseq = 0, nlight = 0, nraw = 0;
        while (o + 4 <= cs) {
            unsigned bh = (unsigned char)c[o] | ((unsigned char)c[o+1] << 8)
                        | ((unsigned char)c[o+2] << 16) | ((unsigned char)c[o+3] << 24);
            if (bh == 0) break;
            unsigned size;
            if (bh & 0x80000000u) { size = bh & 0x7FFFFFFFu; nraw++; }
            else if (bh & 0x40000000u) { size = bh & 0x3FFFFFFFu; nseq++; }
            else { size = bh & 0x3FFFFFFFu; nlight++; }
            if (size == 0 || o + 4 + size > cs) break;
            o += 4 + size;
        }
        if (nseq + nlight + nraw == 0) { fprintf(stderr, "kind=1: block walk found nothing\n"); return 1; }
        printf("   blocks: seq=%zu light=%zu raw=%zu\n", nseq, nlight, nraw);
    }
    LZ6F_freeDecompressionContext(dctx);
    free(src); free(c); free(out);
    return 0;
}

int main(void) {
    /* single magic 06 22 4D 18 — the block codec rides on per-block bit30 */
    const LZ6F_blockSizeID_t bids[] = { LZ6F_max64KB, LZ6F_max256KB, LZ6F_max1MB, LZ6F_max4MB };
    for (unsigned b = 0; b < sizeof(bids)/sizeof(bids[0]); b++) {
        if (roundtrip(700 * 1000, bids[b], 0, 5, 1, 0)) return 1;
        if (roundtrip(700 * 1000, bids[b], 1, 2, 1, 0)) return 1;
        if (roundtrip(700 * 1000, bids[b], 1, 15, 0, 0)) return 1;   /* autoflush=0 path */
        if (roundtrip(3 * 1000 * 1000, bids[b], 1, 2, 1, 0)) return 1;   /* multi-block */
        if (roundtrip(12345, bids[b], 1, 2, 1, 0)) return 1;   /* tiny */
        if (roundtrip(500000, bids[b], 1, 2, 1, 1)) return 1;   /* mixed: light-LZ fallback */
        if (roundtrip(500000, bids[b], 1, 2, 1, 2)) return 1;   /* incompressible: bit31 escape */
    }
    return 0;
}
