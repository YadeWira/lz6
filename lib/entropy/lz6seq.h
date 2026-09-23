/*
 * lz6seq.h - Sequence codec: lz6 matcher + FSE entropy (self-contained).
 *
 * Compresses a buffer by running lz6's HC match finder and coding the
 * resulting sequences with an entropy stage:
 *   - lit_len / match_len / offset symbols (log buckets, offset repcodes
 *     rep0/1/2 folded into the offset alphabet) as table-ANS states
 *   - every extra bit (ll/ml extras, offset = bucket + raw bits) in the
 *     same backward bitstream as the states
 *   - literals: raw / Huffman / rANS order-0 / rANS order-1 (16 or 256
 *     contexts), picked per block
 *   - whole-block transforms: byte-plane lanes, PRNG seed regeneration
 *
 * This is the "entropy backend" extracted from the ozip project so the
 * whole lz6->entropy pipeline lives in one repository.
 *
 * Stream layout (per block):
 *   [flags:1][isize:4]
 *   [lit_mode:1][lit_count:vlq][literal payload]
 *   [seq_count:4][ll table][ml table][of table]
 *   [bits len:4][8 pad bytes][backward bitstream]
 * The byte-exact specification is the "Seq Stream Format" page of the
 * project wiki.
 */
#ifndef LZ6SEQ_H
#define LZ6SEQ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compress `src` (srcSize bytes) into `dst` (dstCap bytes).
 * `level` selects the lz6 HC level (1..15; 9 default-ish).
 * Returns bytes written, or 0 on failure / if dst is too small.
 * Guaranteed to fit if dstCap >= LZ6_COMPRESSBOUND(srcSize) + 65536.
 */
size_t LZ6_compress_seq(const char* src, size_t srcSize,
                        char* dst, size_t dstCap, int level);

/* Decompress a stream produced by LZ6_compress_seq.
 * `dst` must have room for the original size (recovered from the
 * stream). Returns bytes written, or 0 on error.
 */
size_t LZ6_decompress_seq(const char* src, size_t srcSize,
                          char* dst, size_t dstCap);

#ifdef __cplusplus
}
#endif

#endif /* LZ6SEQ_H */
