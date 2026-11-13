/* SPDX-License-Identifier: MIT */
/*
 * encvec.c - one test vector from the real encoder.
 *   encvec IN OFFSET LENGTH LEVEL BLOCKMASK LITMASK OUT.seq OUT.orig
 * Takes LENGTH bytes of IN from OFFSET (LENGTH 0: to the end; IN may also be "prng:SEED:SHIFT", the
 * bytes random() >> SHIFT of glibc after srandom(SEED), or one of the generated inputs below, which
 * depend on nothing but this file; OFFSET is then ignored and LENGTH is required), compresses them as ONE seq block with
 * LZ6_compress_seq_opt(level, blockMask, litMask) (masks in C notation: 0x0F, 0x1EA, ...), checks the
 * round trip with LZ6_decompress_seq and writes the block and the original.
 * Generated inputs (a 64-bit LCG, top bits):
 *   lcg:SEED                  bytes that nothing compresses (and that are not glibc's random())
 *   markov:SEED:ALPHA:SPREAD  bytes below ALPHA, each one of SPREAD successors of the byte before it
 *   markov16:SEED:ALPHA:SPREAD the same, the successors chosen by the high nibble of the byte before
 *   walk16:SEED / walk32:SEED little-endian 16-bit / 32-bit samples of a smooth random walk
 * Built with -DSEQVEC_NO_OPT it calls LZ6_compress_seq instead and ignores the masks: that is how the
 * format 1 vectors are made, against the library of v1.7.1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lz6seq.h"

static int put(const char* name, const char* d, size_t n)
{
    FILE* f = fopen(name, "wb");
    if (!f || fwrite(d, 1, n, f) != n || fclose(f)) { perror(name); return 0; }
    return 1;
}
static unsigned long long g_x;
static unsigned rnd(void) { g_x = g_x * 6364136223846793005ULL + 1442695040888963407ULL; return (unsigned)(g_x >> 40); }   /* 24 bits */
static int gen(const char* spec, unsigned char* d, long n)
{
    unsigned long long seed = 0;
    unsigned alpha = 0, spread = 0, b = 0;
    if (sscanf(spec, "lcg:%llu", &seed) == 1) {
        g_x = seed;
        for (long i = 0; i < n; i++) d[i] = (unsigned char)(rnd() >> 16);
    } else if (sscanf(spec, "markov:%llu:%u:%u", &seed, &alpha, &spread) == 3 && alpha && alpha <= 256 && spread) {
        g_x = seed;
        for (long i = 0; i < n; i++) { b = (b * 7 + 3 + rnd() % spread) % alpha; d[i] = (unsigned char)b; }
    } else if (sscanf(spec, "markov16:%llu:%u:%u", &seed, &alpha, &spread) == 3 && alpha && alpha <= 256 && spread) {
        g_x = seed;
        for (long i = 0; i < n; i++) { b = ((b >> 4) * 37 + 11 + rnd() % spread) % alpha; d[i] = (unsigned char)b; }
    } else if (sscanf(spec, "walk16:%llu", &seed) == 1) {
        int v = 20000;
        g_x = seed;
        for (long i = 0; i < n; i++) { if (!(i & 1)) v += (int)(rnd() % 41) - 20; d[i] = (unsigned char)((unsigned)v >> (8 * (i & 1))); }
    } else if (sscanf(spec, "walk32:%llu", &seed) == 1) {
        int v = 1 << 28;
        g_x = seed;
        for (long i = 0; i < n; i++) { if (!(i & 3)) v += (int)(rnd() % 2001) - 1000; d[i] = (unsigned char)((unsigned)v >> (8 * (i & 3))); }
    } else return 0;
    return 1;
}

int main(int argc, char** argv)
{
    if (argc != 9) { fprintf(stderr, "usage: encvec IN OFFSET LENGTH LEVEL BLOCKMASK LITMASK OUT.seq OUT.orig\n"); return 2; }
    const long off = strtol(argv[2], NULL, 0);
    long len = strtol(argv[3], NULL, 0);
    const int level = atoi(argv[4]);
    const unsigned bm = (unsigned)strtoul(argv[5], NULL, 0), lm = (unsigned)strtoul(argv[6], NULL, 0);
    char* src;
    if (strchr(argv[1], ':') && argv[1][0] != '/' && argv[1][0] != '.') {
        if (len <= 0) { fprintf(stderr, "a generated input needs a LENGTH\n"); return 2; }
        src = (char*)malloc((size_t)len + 1);
        if (!src) return 2;
        if (argv[1][0] == 'p') {
            unsigned seed = 0; int shift = 0;
            if (sscanf(argv[1] + 5, "%u:%d", &seed, &shift) != 2) { fprintf(stderr, "prng:SEED:SHIFT\n"); return 2; }
            srandom(seed);
            for (long i = 0; i < len; i++) src[i] = (char)(random() >> shift);
        } else if (!gen(argv[1], (unsigned char*)src, len)) { fprintf(stderr, "%s: unknown generator\n", argv[1]); return 2; }
    } else {
        FILE* f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 2; }
        fseek(f, 0, SEEK_END);
        const long fl = ftell(f);
        if (off > fl) { fprintf(stderr, "%s: offset beyond the end\n", argv[1]); return 2; }
        if (len == 0 || len > fl - off) len = fl - off;
        fseek(f, off, SEEK_SET);
        src = (char*)malloc((size_t)len + 1);
        if (!src || fread(src, 1, (size_t)len, f) != (size_t)len) { fprintf(stderr, "read error\n"); return 2; }
        fclose(f);
    }
    const size_t cap = (size_t)len + (size_t)len / 16 + 70000;
    char* dst = (char*)malloc(cap);
    char* back = (char*)malloc((size_t)len + 1);
    if (!dst || !back) return 2;
#ifdef SEQVEC_NO_OPT
    (void)bm; (void)lm;
    const size_t c = LZ6_compress_seq(src, (size_t)len, dst, cap, level);
#else
    const size_t c = LZ6_compress_seq_opt(src, (size_t)len, dst, cap, level, bm, lm);
#endif
    if (c == 0) { fprintf(stderr, "compression failed\n"); return 1; }
    const size_t r = LZ6_decompress_seq(dst, c, back, (size_t)len);
    if (r != (size_t)len || memcmp(back, src, (size_t)len)) { fprintf(stderr, "ROUND TRIP FAILED\n"); return 1; }
    if (!put(argv[7], dst, c) || !put(argv[8], src, (size_t)len)) return 2;
    return 0;
}
