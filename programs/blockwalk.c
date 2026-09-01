/* blockwalk: parse a seq frame's block headers and decode each block
 * individually to find the failing one. Diagnostic tool for the frame
 * layer — caught the light-LZ payload clobber and the segmentation
 * block structure. Usage: blockwalk frame.lz6 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "entropy/lz6seq.h"
#include "lz6frame.h"

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* d = malloc((size_t)fsz);
    if (fread(d, 1, (size_t)fsz, f) != (size_t)fsz) return 1;
    fclose(f);
    size_t o = 7;   /* magic + FLG + BD + HC (no content size in this frame) */
    int bi = 0;
    while (o + 4 <= (size_t)fsz) {
        unsigned bh = d[o] | (d[o+1] << 8) | (d[o+2] << 16) | ((unsigned)d[o+3] << 24);
        if (bh == 0) { printf("block %d: ENDMARK at %ld\n", bi, o); break; }
        unsigned size;
        int kind;   /* 0 lz, 1 seq, 2 raw */
        if (bh & 0x80000000u) { size = bh & 0x7FFFFFFFu; kind = 2; }
        else if (bh & 0x40000000u) { size = bh & 0x3FFFFFFFu; kind = 1; }
        else { size = bh & 0x3FFFFFFFu; kind = 0; }
        if (size == 0 || o + 4 + size > (size_t)fsz) { printf("block %d: BAD size %u at %ld\n", bi, size, o); break; }
        unsigned char* payload = d + o + 4;
        if (kind == 1) {
            /* seq block: flags + isize */
            unsigned flags = payload[0];
            unsigned isize = payload[1] | (payload[2] << 8) | (payload[3] << 16) | ((unsigned)payload[4] << 24);
            unsigned char* out = malloc((size_t)isize + 65536);
            size_t r = LZ6_decompress_seq((const char*)payload, size, (char*)out, (size_t)isize + 65536);
            printf("block %d: seq isize=%u csize=%u flags=%u -> %s (%zu decoded)\n",
                   bi, isize, size, flags, (r == (size_t)isize && r) ? "OK" : "FAIL", r);
            free(out);
        } else if (kind == 2) {
            printf("block %d: raw size=%u\n", bi, size);
        } else {
            printf("block %d: LZ-coded size=%u (needs frame LZ decoder)\n", bi, size);
        }
        o += 4 + size;
        bi++;
    }
    free(d);
    return 0;
}
