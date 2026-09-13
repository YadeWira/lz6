/* mk_legacy_fixture.c - build a bakeoff/compat_LZ6S1_*.lz6s1 fixture.
 *
 * The LZ6S1 envelope (magic "LZ6S1", then per chunk u32le csize | u32le
 * usize | payload, terminated by 8 zero bytes) is a decode-only container:
 * nothing in the encoder writes it any more, but the CLI still has to read
 * streams produced by pre frame-integration builds. This tool rebuilds the
 * fixture with the CURRENT seq block format so the shim's container path
 * stays covered after a block-format change.
 *
 * Usage: mk_legacy_fixture LEVEL INFILE OUTFILE
 * Build: cc -O2 -std=c99 -Ilib bakeoff/mk_legacy_fixture.c lib/lz6.o \
 *          lib/lz6hc.o lib/entropy/lz6fse.o lib/entropy/lz6rc.o \
 *          lib/entropy/lz6seq.o lib/entropy/lz6huf.o -lm -o mk_legacy_fixture
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "entropy/lz6seq.h"

static void w32(FILE* f, unsigned v) {
    fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f);
    fputc((v >> 16) & 0xFF, f); fputc((v >> 24) & 0xFF, f);
}

int main(int argc, char** argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s LEVEL INFILE OUTFILE\n", argv[0]); return 1; }
    int level = atoi(argv[1]);
    FILE* fi = fopen(argv[2], "rb");
    if (!fi) { perror(argv[2]); return 1; }
    fseek(fi, 0, SEEK_END); long n = ftell(fi); fseek(fi, 0, SEEK_SET);
    char* src = malloc((size_t)n);
    char* dst = malloc((size_t)n + (size_t)n / 2 + 65536);
    if (!src || !dst) { fprintf(stderr, "oom\n"); return 1; }
    if (fread(src, 1, (size_t)n, fi) != (size_t)n) { fprintf(stderr, "short read\n"); return 1; }
    fclose(fi);

    size_t csize = LZ6_compress_seq(src, (size_t)n, dst, (size_t)n + (size_t)n / 2 + 65536, level);
    if (!csize) { fprintf(stderr, "compress failed\n"); return 1; }

    FILE* fo = fopen(argv[3], "wb");
    if (!fo) { perror(argv[3]); return 1; }
    fputs("LZ6S1", fo);
    w32(fo, (unsigned)csize);
    w32(fo, (unsigned)n);
    fwrite(dst, 1, csize, fo);
    w32(fo, 0); w32(fo, 0);
    fclose(fo);
    fprintf(stderr, "wrote %ld bytes (payload %zu)\n", (long)(5 + 8 + (long)csize + 8), csize);
    free(src); free(dst);
    return 0;
}
