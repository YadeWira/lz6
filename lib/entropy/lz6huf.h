/*
 * lz6huf.h — static Huffman coding for literals (zstd HUF-style decode)
 *
 * Encodes `n` symbols over an alphabet of `maxSym+1` codes with a
 * canonical Huffman tree. The bitstream is a single MSB-first stream
 * (cheap encode); decoding uses a direct 2^k lookup table (no renorm,
 * no multiply — just shift + table hit per symbol).
 *
 * Layout (encode output):
 *   [1B k]                     k = max code length (2..16), 0 = empty
 *   [1B nbits]                 bits in the stream (0 if empty)
 *   [nbits/8 + 1 bytes]        bitstream, MSB-first
 *   [256 B]                    code lengths, 1 byte each (0 = unused)
 *   — total 2 + 257 + ceil(nbits/8) bytes
 *
 * For the common case k <= 8 the header is 2+257 bytes; k>8 tables
 * are built but only used when a code exceeds 8 bits.
 */

#ifndef LZ6HUF_H
#define LZ6HUF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build a canonical Huffman code over `counts` (maxSym+1 symbols).
 * Writes [k][256 lengths] into `out`; returns bytes written (257),
 * or 0 on error/oversize. `maxSym` must be <= 255. */
size_t huf_build_header(const unsigned* counts, int maxSym,
                        uint8_t* out, size_t out_cap, int* out_k);

/* Encode `syms[0..n)` with the lengths in `hdr` (huf_build_header
 * output). Writes [4B total][MSB-first stream] into `out`, returns
 * bytes written (4+stream), or 0 on error. */
size_t huf_encode_stream(const uint8_t* hdr, const unsigned* syms, size_t n,
                         uint8_t* out, size_t out_cap);
/* Same, over byte symbols. */
size_t huf_encode_stream8(const uint8_t* hdr, const uint8_t* syms, size_t n,
                          uint8_t* out, size_t out_cap);

/* Decode `n` symbols from `in` = [header][stream] as produced by
 * huf_build_header + huf_encode_stream. Returns bytes consumed, or 0
 * on error. */
size_t huf_decode(const uint8_t* in, size_t in_len, size_t n,
                  uint8_t* out);

/* 4 interleaved streams sharing one code table (hdr from huf_build_header):
 * [4 x ([total bits:4][MSB-first stream])], segments of ceil(n/4) symbols.
 * Returns bytes written, 0 on error. */
size_t huf_encode4_8(const uint8_t* hdr, const uint8_t* syms, size_t n,
                     uint8_t* out, size_t out_cap);

/* Streaming decode: init once, decode chunks on demand (literals are
 * decoded inline into the sequence loop). */
typedef struct {
    int k;                    /* lookup table bits */
    uint16_t* table;          /* 2^k entries: (sym << 4) | len, 0xFFFF = long code */
    uint32_t* table2;         /* 2^k entries: sym1 | sym2<<8 | len<<16 | count<<20,
                                 count 0 = long first code */
    int cnt[17];              /* codes per length (canonical walk) */
    uint8_t sorted[256];
    int off[17];
    int first_code[17];
    const uint8_t* base;      /* MSB-first bitstream, zero-extended past nbytes */
    size_t nbytes;
    size_t bitpos;            /* bits consumed from base */
} huf_dstate;

size_t huf_dec_init(huf_dstate* s, const uint8_t* in, size_t in_len);
int huf_dec_n(huf_dstate* s, uint8_t* out, size_t n);   /* 0 ok, -1 error */
void huf_dec_free(huf_dstate* s);
/* header only (k + 256 code lengths): builds the tables; returns 257 or 0 */
size_t huf_dec_tables(huf_dstate* s, const uint8_t* in, size_t in_len);
/* decode n symbols from the 4-stream layout with tables from
 * huf_dec_tables; returns bytes consumed from in, 0 on error */
size_t huf_decode4(const huf_dstate* t, const uint8_t* in, size_t in_len,
                   uint8_t* out, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* LZ6HUF_H */
