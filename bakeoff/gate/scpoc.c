/* scpoc: a block that ends right after its raw literals, so the 4-byte sequence count does not exist. Before the fix
 * decode_normal_body read it past the end of the stream (heap over-read); the call must now return 0 with no report. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lib/entropy/lz6seq.h"
int main(void){
    const uint8_t blk[] = {0, 4,0,0,0, 0, 4, 'a','b','c','d'};   /* flags isize lit_mode lit_count lits; no sc */
    uint8_t* src = malloc(sizeof blk); memcpy(src, blk, sizeof blk);
    char dst[16];
    size_t r = LZ6_decompress_seq((const char*)src, sizeof blk, dst, 4);
    printf("ret %zu\n", r);
    free(src);
    return 0;
}
