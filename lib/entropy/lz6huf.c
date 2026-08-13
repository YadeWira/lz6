/*
 * lz6huf.c — static Huffman coding for literals
 *
 * Encode: canonical Huffman over the byte alphabet, single MSB-first
 * bitstream (cheap encode). Decode: direct 2^k lookup table for codes
 * up to k=12 bits; codes longer than k (up to 16) fall back to a
 * canonical bit-by-bit walk (rare symbols, negligible cost).
 *
 * Layout:
 *   [1B k]                    max code length used in the table (<=12)
 *   [256 B]                   code length per symbol (0 = unused)
 *   [4B total]                total bits in the stream (LE)
 *   [stream]                  ceil(total/8) bytes, MSB-first
 *
 * k = min(max_code_len, 12). If max_code_len > 16 (pathological
 * Fibonacci-style distribution), huf_build_header returns 0 and the
 * caller falls back to raw storage.
 */

#include "lz6huf.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define HUF_MAX_TABLE_BITS 12
#define HUF_MAX_CODE_BITS  16

/* ------------------------------------------------------------------ */
/* Tree construction                                                   */
/* ------------------------------------------------------------------ */

/* Build code lengths (depth) for a Huffman tree over counts[0..maxSym].
 * Returns the max length, 0 if no symbols. Fills len[]. */
static int build_lengths(const unsigned* counts, int maxSym, uint8_t* len)
{
    int syms[256];
    int n_syms = 0;
    for (int i = 0; i <= maxSym; i++)
        if (counts[i] > 0) syms[n_syms++] = i;
    if (n_syms == 0) return 0;
    if (n_syms == 1) { len[syms[0]] = 1; return 1; }

    unsigned freq[512];
    int parent[512];
    for (int i = 0; i < 512; i++) parent[i] = -1;   /* internal nodes too */
    for (int i = 0; i < n_syms; i++) { freq[i] = counts[syms[i]]; parent[i] = -1; }

    int live[512];
    for (int i = 0; i < n_syms; i++) live[i] = i;
    int n = n_syms;
    int next = n_syms;
    while (n > 1) {
        /* two smallest ROOTS among live[0..n) — roots only, so every
         * node keeps exactly one parent (proper binary tree, Kraft=1) */
        int a = 0, b = 1;
        if (freq[live[1]] < freq[live[0]]) { a = 1; b = 0; }
        for (int i = 2; i < n; i++) {
            if (freq[live[i]] < freq[live[a]]) { b = a; a = i; }
            else if (freq[live[i]] < freq[live[b]]) b = i;
        }
        int na = live[a], nb = live[b];
        parent[na] = next;
        parent[nb] = next;
        freq[next] = freq[na] + freq[nb];
        /* na and nb leave the root set; the merged node joins it.
         * Replace the slot of the larger-index root with the merged
         * node, then drop the other slot from the end (order matters:
         * shrink first, then plant). */
        if (a > b) { int t = a; a = b; b = t; }
        live[b] = live[--n];   /* a < b: bring the last root into b */
        live[a] = next++;      /* merged node takes a's slot */
    }
    (void)n;

    int maxd = 0;
    for (int i = 0; i < n_syms; i++) {
        int d = 0;
        for (int p = parent[i]; p >= 0; p = parent[p]) d++;
        len[syms[i]] = (uint8_t)d;
        if (d > maxd) maxd = d;
    }
    return maxd;
}

/* ------------------------------------------------------------------ */
/* Canonical code assignment (backward, Kraft-safe)                    */
/* ------------------------------------------------------------------ */

/* Assign codes from the longest length down: first[16] = 0, then
 * first[l] = (first[l+1] + count[l+1]) >> 1. Guarantees every code of
 * length l is < 2^l (Kraft = 1), so table fills never overflow. */
static void canonical_codes(const uint8_t* len, int maxSym, uint16_t* code)
{
    int count[17];
    for (int i = 0; i <= 16; i++) count[i] = 0;
    for (int i = 0; i <= maxSym; i++) count[len[i]]++;
    int first[17];
    first[16] = 0;
    for (int l = 15; l >= 1; l--)
        first[l] = (first[l + 1] + count[l + 1]) >> 1;
    for (int i = 0; i <= maxSym; i++) {
        int l = len[i];
        if (l > 0) code[i] = (uint16_t)(first[l]++);
    }
}

/* ------------------------------------------------------------------ */
/* Header + encode                                                     */
/* ------------------------------------------------------------------ */

size_t huf_build_header(const unsigned* counts, int maxSym,
                        uint8_t* out, size_t out_cap, int* out_k)
{
    if (maxSym > 255) return 0;
    uint8_t len[256];
    memset(len, 0, sizeof(len));
    int maxd = build_lengths(counts, maxSym, len);
    if (maxd == 0) return 0;
    if (maxd > HUF_MAX_CODE_BITS) return 0;   /* pathological: caller falls back */
    if (out_cap < 257) return 0;

    int k = maxd < HUF_MAX_TABLE_BITS ? maxd : HUF_MAX_TABLE_BITS;
    out[0] = (uint8_t)k;
    memcpy(out + 1, len, 256);
    if (out_k) *out_k = k;
    return 257;
}

/* Encode syms[0..n) with the lengths in hdr (huf_build_header output).
 * Writes [4B total][stream] at `out`; the caller appends this after the
 * 257-byte header. Returns bytes written (4 + stream), or 0 on error. */
size_t huf_encode_stream(const uint8_t* hdr, const unsigned* syms, size_t n,
                         uint8_t* out, size_t out_cap)
{
    int k = hdr[0];
    const uint8_t* len = hdr + 1;
    (void)k;
    uint16_t code[256];
    canonical_codes(len, 255, code);

    size_t total = 0;
    for (size_t i = 0; i < n; i++) total += len[syms[i]];
    size_t nbytes = (total + 7) / 8;
    if (nbytes == 0) nbytes = 1;
    if (out_cap < nbytes + 4) return 0;

    out[0] = (uint8_t)(total);
    out[1] = (uint8_t)(total >> 8);
    out[2] = (uint8_t)(total >> 16);
    out[3] = (uint8_t)(total >> 24);

    uint8_t* p = out + 4;
    uint32_t acc = 0;
    int nbits = 0;
    for (size_t i = 0; i < n; i++) {
        int s = (int)syms[i];
        int l = len[s];
        acc = (acc << l) | code[s];
        nbits += l;
        while (nbits >= 8) {
            nbits -= 8;
            *p++ = (uint8_t)(acc >> nbits);
        }
    }
    if (nbits > 0) *p++ = (uint8_t)(acc << (8 - nbits));
    return 4 + (size_t)(p - (out + 4));
}

/* ------------------------------------------------------------------ */
/* Decode                                                              */
/* ------------------------------------------------------------------ */

size_t huf_decode(const uint8_t* in, size_t in_len, size_t n, uint8_t* out)
{
    if (in_len < 261) return 0;
    int k = in[0];
    if (k == 0 || k > HUF_MAX_TABLE_BITS) return 0;
    const uint8_t* len = in + 1;
    uint32_t total = (uint32_t)in[257] | ((uint32_t)in[258] << 8) |
                     ((uint32_t)in[259] << 16) | ((uint32_t)in[260] << 24);
    const uint8_t* stream = in + 261;
    size_t nbytes = (total + 7) / 8;
    if (in_len < 261 + nbytes) return 0;

    /* canonical table for the walk (codes longer than k) */
    int cnt[17];
    uint8_t sorted[256];
    int off[17];
    int first_code[17];
    {
        for (int l = 0; l <= 16; l++) cnt[l] = 0;
        for (int s = 0; s <= 255; s++) cnt[len[s]]++;
        first_code[16] = 0;
        for (int l = 15; l >= 1; l--)
            first_code[l] = (first_code[l + 1] + cnt[l + 1]) >> 1;
        int acc = 0;
        for (int l = 1; l <= 16; l++) { off[l] = acc; acc += cnt[l]; }
        int pos[17];
        memcpy(pos, off, sizeof(pos));
        for (int s = 0; s <= 255; s++)
            if (len[s] > 0) sorted[pos[len[s]]++] = (uint8_t)s;
    }

    uint16_t code[256];
    canonical_codes(len, 255, code);

    /* lookup table: 2^k entries of (sym << 4) | len; 0xFFFF = long code */
    size_t tsize = (size_t)1 << k;
    uint16_t* table = (uint16_t*)malloc(tsize * sizeof(uint16_t));
    if (!table) return 0;
    for (size_t i = 0; i < tsize; i++) table[i] = 0xFFFF;
    for (int s = 0; s <= 255; s++) {
        int l = len[s];
        if (l == 0) continue;
        if (l <= k) {
            int shift = k - l;
            uint16_t v = (uint16_t)((s << 4) | l);
            uint32_t base = (uint32_t)code[s] << shift;
            for (uint32_t sub = 0; sub < (1u << shift); sub++) {
                if ((base | sub) >= tsize) { fprintf(stderr, "OOB s=%d l=%d code=%d shift=%d idx=%u tsize=%zu\n", s, l, code[s], shift, base|sub, tsize); free(table); return 0; }
                table[base | sub] = v;
            }
        } else {
            /* prefix of the long code fills exactly one slot */
            table[(uint32_t)code[s] >> (l - k)] = 0xFFFF;
        }
    }

    /* fast table-only path: when every code length <= k (the caller's
     * encode gate guarantees this for k=hk), the 0xFFFF long-code slot is
     * never hit and the decode is a tight branch-predictable loop with a
     * 64-bit accumulator (refill 1 byte when below 24 bits). */
    uint64_t acc = 0;
    int nbits = 0;
    const uint8_t* p = stream;
    const uint8_t* pend = stream + nbytes;
    const uint32_t kmask = (1u << k) - 1;
    while (nbits < 32 && p < pend) { acc = (acc << 8) | *p++; nbits += 8; }
    size_t i = 0;
    for (; i < n; i++) {
        while (nbits < 24) { acc = (acc << 8) | (p < pend ? *p++ : 0); nbits += 8; }
        int shift = nbits - k;
        uint16_t v = table[(unsigned)((acc >> shift) & kmask)];
        int l = v & 15;
        if (l != 0 && l <= k) {
            out[i] = (uint8_t)(v >> 4);
            nbits -= l;
        } else {
            /* long code: canonical bit-by-bit walk (rare) */
            int codev = 0;
            int l2 = 0;
            for (l2 = 1; l2 <= HUF_MAX_CODE_BITS; l2++) {
                if (nbits == 0) {
                    acc = (acc << 8) | (p < pend ? *p++ : 0);
                    nbits = 8;
                }
                codev = (codev << 1) | (int)((acc >> (nbits - 1)) & 1);
                nbits--;
                int d = codev - first_code[l2];
                if (d >= 0 && d < cnt[l2]) {
                    out[i] = sorted[off[l2] + d];
                    break;
                }
            }
            if (l2 > HUF_MAX_CODE_BITS) { free(table); return 0; }
        }
    }
    free(table);
    return 261 + nbytes;
}
