/* SPDX-License-Identifier: MIT */
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
 * `level`: 1..13, higher compresses more and is slower (1 is the entry level, about the ratio of the LZMA family's first;
 * 7 is the default when `level` is 0; 14 and 15 are 13 for now); -1 and -2 are the fast tier (-2 faster than -1, the CLI's
 * --fast=1 and --fast=2). Before v1.6.12-pre the scale was 1..15 with 1 and 2 the fast tier: its levels 3..15 are 1..13 here,
 * its 2 and 1 are -1 and -2.
 * Returns bytes written, or 0 on failure / if dst is too small.
 * Guaranteed to fit if dstCap >= LZ6_COMPRESSBOUND(srcSize) + 65536.
 */
size_t LZ6_compress_seq(const char* src, size_t srcSize,
                        char* dst, size_t dstCap, int level);

/* The restricted encoder: LZ6_compress_seq that writes only the parts of the format the caller allows. It is for a
 * decoder that implements a subset of the seq format (a port in another language, a small or verified decoder): the
 * streams it gets hold nothing else.
 *
 * `blockMask` says which block kinds may be written besides the two that are always allowed, the normal block
 * (format 2, flags 0x20) and the stored block (flags 0x01):
 *   LZ6SEQ_BLOCK_X86          x86-filtered blocks (flags 0x10)
 *   LZ6SEQ_BLOCK_PLANE        byte-plane blocks (flags 0x08) with their three lane modes: raw (0), a seq block (1),
 *                             a Huffman stream (2). No lane has the delta bit
 *   LZ6SEQ_BLOCK_PLANE_DELTA  the lanes of a plane block may carry the delta bit 0x80. Has no effect without
 *                             LZ6SEQ_BLOCK_PLANE
 *   LZ6SEQ_BLOCK_PRNG         PRNG regeneration blocks (flags 0x04)
 * `litMask` says which literal modes may be written besides raw (mode 0), which is always allowed: an OR of
 * LZ6SEQ_LIT(mode) for the modes 1 (rANS order-0), 3 and 6 (rANS order-1 with 256 and 16 contexts), 5 (Huffman),
 * 7 (Huffman in 4 streams) and 8 (literal epochs: several sections, each in an allowed mode).
 * Bits outside LZ6SEQ_BLOCK_ALL and LZ6SEQ_LIT_ALL are ignored.
 *
 * The restriction holds at every depth: for the block inside an x86 block, for the seq blocks of a plane block's
 * lanes, for the sections inside literal epochs. Everything else of format 2 is always in use (the sequence epochs,
 * the bit-packed table headers, the LEB128 counts): a decoder for these streams reads normal format 2 blocks.
 *
 * With every bit set this is LZ6_compress_seq: LZ6_compress_seq(s, n, d, cap, level) is
 * LZ6_compress_seq_opt(s, n, d, cap, level, LZ6SEQ_BLOCK_ALL, LZ6SEQ_LIT_ALL) and writes the same bytes.
 * With less, each literal section is coded in the best mode that is left (without mode 7 the single Huffman stream
 * of mode 5 takes any count; without both, order-0 rANS at every level; with nothing, raw; mode 7 is still only
 * written from 4096 literals up and modes 3 and 6 only where LZ6_compress_seq tries them), and data that would
 * have been a block of a kind that is not allowed becomes a normal or a stored block. A mask costs ratio, a lot on
 * the data its block kind is made for (x86 code, 16-bit and float samples, text without Huffman or rANS literals):
 * nothing is promised about the size, only that the output is a valid stream of the allowed parts, which every
 * LZ6_decompress_seq from v2.0.0 on decodes. As with LZ6_compress_seq, the bytes written for a given input may
 * change between versions.
 *
 * The restriction is a stable part of the API: the macros keep their values and their meaning, and a feature that a
 * later version adds to the format gets a bit of its own and is not written without it (a caller that names its bits,
 * instead of passing LZ6SEQ_BLOCK_ALL or LZ6SEQ_LIT_ALL, keeps getting streams of the parts it named).
 * The masks are parameters and nothing is kept between calls: concurrent calls with different masks are safe
 * (after LZ6_seq_init, as for every function here).
 * Returns and `level` as LZ6_compress_seq; the same bound on dstCap holds under every mask.
 */
#define LZ6SEQ_BLOCK_X86         0x01u   /* x86-filtered blocks (flag 0x10) */
#define LZ6SEQ_BLOCK_PLANE       0x02u   /* byte-plane blocks (flag 0x08), lanes without the delta bit */
#define LZ6SEQ_BLOCK_PLANE_DELTA 0x04u   /* plane lanes may carry the delta bit 0x80 (needs LZ6SEQ_BLOCK_PLANE) */
#define LZ6SEQ_BLOCK_PRNG        0x08u   /* PRNG regeneration blocks (flag 0x04) */
#define LZ6SEQ_BLOCK_ALL         0x0Fu
#define LZ6SEQ_LIT(mode)         (1u << (mode))   /* literal modes 1, 3, 5, 6, 7, 8; mode 0 (raw) is always allowed */
#define LZ6SEQ_LIT_ALL           (LZ6SEQ_LIT(1)|LZ6SEQ_LIT(3)|LZ6SEQ_LIT(5)|LZ6SEQ_LIT(6)|LZ6SEQ_LIT(7)|LZ6SEQ_LIT(8))
size_t LZ6_compress_seq_opt(const char* src, size_t srcSize,
                            char* dst, size_t dstCap, int level,
                            unsigned blockMask, unsigned litMask);

/* Builds the codec's lookup tables. Called lazily by the functions below, which
 * is a data race if the first calls come from several threads at once: call it
 * once before starting threads (LZ6F_create*Context do). Idempotent. */
void LZ6_seq_init(void);

/* Decompress a stream produced by LZ6_compress_seq or LZ6_compress_seq_opt.
 * `dst` must have room for the original size (recovered from the
 * stream). Returns bytes written, or 0 on error.
 */
size_t LZ6_decompress_seq(const char* src, size_t srcSize,
                          char* dst, size_t dstCap);

#ifdef __cplusplus
}
#endif

#endif /* LZ6SEQ_H */
