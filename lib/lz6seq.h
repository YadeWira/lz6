/*
 * lz6seq.h - Sequence codec: lz6 matcher + FSE entropy (self-contained).
 *
 * Compresses a buffer by running lz6's HC match finder and coding the
 * resulting sequences with an FSE/rANS entropy coder:
 *   - lit_len / match_len / offset-bucket as FSE symbol streams
 *   - repcodes (rep0/1/2) for repeat offsets
 *   - literals: FSE order-0 (lit_mode=1) with raw fallback
 *   - offset residual: log2-bucket + raw bits
 *
 * This is the "entropy backend" extracted from the ozip project so the
 * whole lz6→entropy pipeline lives in one repository.
 *
 * Stream layout (per block):
 *   [flags:1][isize:4]
 *   [lit_mode:1][lit_count:vlq][lit_csize:vlq][lit_data]  (lit_mode 0=raw,1=fse)
 *   [seq_count:4]
 *   [ll FSE stream][ml FSE stream][of FSE stream]
 *   [rep_flags: ceil(sc*2/8)]
 *   [extra bits: ll/ml interleaved]
 *
 * The of_syms are log2 buckets (0..24); the residual is split into
 * top8 (FSE per bucket, [1B bucket][4B size][stream]... terminated by
 * [0][0]) + low raw bits ([4B size][bytes]).
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
