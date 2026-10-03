/* hufpoc: a Huffman header whose 256 six-bit lengths oversubscribe the code space (k = 5). Before the fix the long-code branch of
 * huf_dec_tables stored its table slot at an index >= 2^k (heap write out of bounds); the call must now return 0 with no report. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lib/entropy/lz6huf.h"
int main(void){
    uint8_t in[400]; memset(in,0,sizeof in);
    in[0]=5;              /* k = 5 */
    for(int i=0;i<256;i++) in[1+i]=6;   /* oversubscribed: 256 codes of 6 bits */
    huf_dstate s;
    size_t r = huf_dec_tables(&s,in,sizeof in);
    printf("huf_dec_tables returned %zu\n", r);
    return 0;
}
