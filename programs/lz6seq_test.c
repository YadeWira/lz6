/* lz6seq_test.c - round-trip test for the lz6 sequence codec */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/lz6seq.h"

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s file [level]\n", argv[0]); return 1; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char* in = malloc(sz);
    fread(in, 1, sz, f);
    fclose(f);
    int level = argc > 2 ? atoi(argv[2]) : 15;

    size_t cap = (size_t)sz + (size_t)sz / 2 + 65536;
    char* comp = malloc(cap);
    char* dec = malloc(sz + 16);
    size_t csz = LZ6_compress_seq(in, (size_t)sz, comp, cap, level);
    if (!csz) { fprintf(stderr, "compress failed\n"); return 1; }
    size_t dsz = LZ6_decompress_seq(comp, csz, dec, (size_t)sz + 16);
    int ok = dsz == (size_t)sz && memcmp(dec, in, sz) == 0;
    printf("%-12s %8ld -> %8zu  (%.2f%%)  rt=%s\n",
           argv[1], sz, csz, 100.0 * csz / sz, ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
