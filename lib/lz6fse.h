/*
 * fse.h - Finite State Entropy (ANS) entropy coder.
 *
 * Implements Streaming rANS: 32-bit state, byte renormalization.
 * - O(1) per symbol decode via symbol lookup table of size 2^L.
 * - O(1) per symbol encode via division/modulo.
 * - Symmetric semantics to tANS; produces near-identical ratios.
 *
 * Usage:
 *   fse_state s; fse_enc_init(&s, ...);
 *   fse_enc_symbol(&s, freq[s], cumul[s], writer);
 *   fse_enc_flush(&s, writer);
 *   ...
 *   uint32_t x = fse_dec_init(reader);  // x >= RANS_L
 *   sym = fse_dec_lookup(x);
 *   fse_dec_advance(&x, freq[sym], cumul[sym], reader);
 *
 * Public API below composes the above into a single call.
 */

#ifndef OZIP_FSE_H
#define OZIP_FSE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public API: encode/decode a stream of symbols (0..255). */
/* Compress `n` symbols (integers in [0,maxSym]) into `out_cap` bytes.
 * Returns #bytes written, or 0 on error.
 * `table_size_out` returns the size of the table description header
 *          (frequency table written before the bitstream).
 *
 * The encoder uses Lemire's 64×64→128-bit multiplicative inverse to
 * replace the per-symbol `x / f` and `x % f` instructions; reciprocal
 * tables are derived internally from the normalized freq. The
 * `inv_M`/`inv_l` parameters are accepted only for API backward
 * compatibility and are ignored. */
size_t fse_encode(const unsigned* symbols, size_t n,
                  int maxSym,
                  uint8_t* out, size_t out_cap,
                  const uint64_t* inv_M, int inv_l,
                  size_t* table_size_out);

/* Inverse of fse_encode. Reads `in_len` bytes; writes `n` symbols into `out`.
 * Caller supplies `n` (recovered from container) and `maxSym`.
 * Returns 0 on success, non-zero on error. */
int    fse_decode(const uint8_t* in, size_t in_len,
                  size_t n, int maxSym,
                  unsigned* out);

/* Table-only: write the normalized frequency table to `out` for `out_cap`.
 * Counts are normalized to 2^L (powers of two) automatically.
 * Returns bytes written for the table. */
size_t fse_write_table(const unsigned counts[256], int maxSym,
                       uint8_t* out, size_t out_cap);

/* Read a normalized table written by fse_write_table.
 * Fills `counts`, `maxSym_out`, and `L_bits_out`. Returns bytes consumed. */
size_t fse_read_table(const uint8_t* in, size_t in_len,
                      unsigned counts[256], int* maxSym_out, int* L_bits_out);

#ifdef __cplusplus
}
#endif

#endif /* OZIP_FSE_H */