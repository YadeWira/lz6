/* lz6seqcli.c - simple CLI for the lz6 sequence codec (LZ6_compress_seq).
 * Usage:
 *   lz6seq c <in> <out> [level]    compress
 *   lz6seq d <in> <out>            decompress
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/lz6seq.h"

static unsigned char* read_file(const char* path, long* size) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* buf = malloc(*size ? (size_t)*size : 1);
    if (*size && fread(buf, 1, (size_t)*size, f) != (size_t)*size) { perror("read"); free(buf); fclose(f); return NULL; }
    fclose(f);
    return buf;
}

static int write_file(const char* path, const void* data, size_t size) {
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    if (size && fwrite(data, 1, size, f) != size) { perror("write"); fclose(f); return 1; }
    fclose(f);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s c <in> <out> [level] | %s d <in> <out>\n", argv[0], argv[0]);
        return 1;
    }
    long sz;
    unsigned char* in = read_file(argv[2], &sz);
    if (!in) return 1;

    if (argv[1][0] == 'c') {
        int level = argc > 4 ? atoi(argv[4]) : 15;
        size_t cap = (size_t)sz + (size_t)sz / 2 + 65536;
        unsigned char* out = malloc(cap);
        size_t csz = LZ6_compress_seq((const char*)in, (size_t)sz, (char*)out, cap, level);
        if (!csz) { fprintf(stderr, "compress failed\n"); return 1; }
        printf("%ld -> %zu bytes (%.2f%%)\n", sz, csz, 100.0 * csz / sz);
        int rc = write_file(argv[3], out, csz);
        free(out); free(in);
        return rc;
    } else if (argv[1][0] == 'd') {
        /* output size = original size, stored as isize in the stream */
        size_t cap = (size_t)sz * 4 + (1 << 20);
        unsigned char* out = malloc(cap);
        size_t dsz = LZ6_decompress_seq((const char*)in, (size_t)sz, (char*)out, cap);
        if (!dsz) { fprintf(stderr, "decompress failed\n"); return 1; }
        int rc = write_file(argv[3], out, dsz);
        free(out); free(in);
        return rc;
    }
    fprintf(stderr, "unknown mode %s\n", argv[1]);
    return 1;
}
