/* SPDX-License-Identifier: MIT */
/*
 * decchk.c - decodes a vector with the library it is linked against and compares with the original.
 *   decchk FILE.seq FILE.orig
 * Prints "OK <bytes>" (exit 0) or "FAIL <reason>" (exit 1). An empty original is a special case of the
 * API: LZ6_decompress_seq returns 0 both for an empty block and for a block it rejects, so the answer
 * for it is "OK 0 (0 is also the error value)".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lz6seq.h"

static char* slurp(const char* name, size_t* n)
{
    FILE* f = fopen(name, "rb");
    if (!f) { perror(name); exit(2); }
    fseek(f, 0, SEEK_END);
    const long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* d = (char*)malloc((size_t)l + 1);
    if (!d || fread(d, 1, (size_t)l, f) != (size_t)l) { fprintf(stderr, "%s: read error\n", name); exit(2); }
    fclose(f);
    *n = (size_t)l;
    return d;
}
int main(int argc, char** argv)
{
    if (argc != 3) { fprintf(stderr, "usage: decchk FILE.seq FILE.orig\n"); return 2; }
    size_t cn, on;
    char* c = slurp(argv[1], &cn);
    char* o = slurp(argv[2], &on);
    char* out = (char*)malloc(on + 64);
    if (!out) return 2;
    const size_t r = LZ6_decompress_seq(c, cn, out, on);
    int rc = 0;
    if (r != on) { printf("FAIL returned %lu, original is %lu bytes\n", (unsigned long)r, (unsigned long)on); rc = 1; }
    else if (memcmp(out, o, on)) { printf("FAIL output differs\n"); rc = 1; }
    else printf(on ? "OK %lu\n" : "OK %lu (0 is also the error value)\n", (unsigned long)on);
    free(c); free(o); free(out);
    return rc;
}
