/* rpzero.c - regression test for the repeat-offset history rule of the seq decoder.
 *
 * Rule (wiki Seq-Stream-Format): every offset coded EXPLICITLY (code < 191) is pushed onto the rep stack, whatever its value.
 * The encoder never emits an explicit offset equal to rep0 (it codes rep0 instead), so only a hand-built or foreign stream
 * can contain one; this pins what a decoder must do with it, so that two decoders (this C one, a future asm one) cannot
 * silently diverge. (The reference decoder before v1.6.11 skipped the push when the value equalled rep0; keeping that exception
 * cost the decoder 2-4% of its instructions for a case no encoder produces, so the rule was made uniform.) Both blocks were
 * built by hand in the review of the decoder diet; each ends with a rep1 whose target depends on the rule.
 * Decodes each from an EXACT-size source into an EXACT-size destination. Build with ASAN/UBSan like the other gate tests:
 *   cc -O1 -g -fsanitize=address,undefined -I. -Ilib -Ilib/entropy bakeoff/gate/rpzero.c lib/lz6.c lib/lz6hc.c \
 *      lib/entropy/lz6seq.c lib/entropy/lz6fse.c lib/entropy/lz6rc.c lib/entropy/lz6huf.c -lm -o rpzero && ./rpzero */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib/entropy/lz6seq.h"

static const unsigned char blk_min[] = {0x00,0x19,0x00,0x00,0x00,0x00,0x0d,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x6b,0x6c,0x6d,0x05,0x00,0x00,0x00,0x05,0x08,0x00,0x00,0x14,0x06,0x00,0x04,0x06,0x05,0x01,0x06,0x1a,0x06,0xc0,0x0c,0x00,0x02,0x0c,0x00,0x01,0x1c,0x00,0xb7,0x0c,0x0c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xb4,0x66,0x38,0x19};
static const unsigned char blk_nonrep[] = {0x00,0x18,0x00,0x00,0x00,0x00,0x0c,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x6b,0x6c,0x05,0x00,0x00,0x00,0x05,0x08,0x00,0x00,0x1a,0x00,0x05,0x06,0x05,0x01,0x06,0x1a,0x05,0xc0,0x06,0x06,0x00,0x01,0x0e,0x00,0xba,0x06,0x0c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xe4,0x34,0x26,0x05};
static int check(const char* name, const unsigned char* b, size_t n, const char* want)
{
    const size_t wn = strlen(want);
    unsigned char* e = (unsigned char*)malloc(n); char* d = (char*)malloc(wn);
    memcpy(e, b, n);
    const size_t r = LZ6_decompress_seq((const char*)e, n, d, wn);
    const int ok = r == wn && memcmp(d, want, wn) == 0;
    if (!ok) fprintf(stderr, "FAIL %s: rc=%zu out=%.*s (want %zu '%s')\n", name, r, (int)(r < wn ? r : wn), d, wn, want);
    free(e); free(d);
    return ok;
}
int main(void)
{
    LZ6_seq_init();
    const int ok = check("explicit-offset-equals-rep0 (a)", blk_min, sizeof blk_min, "abcdefghdefiefgjdefkefglm") &
                   check("explicit-offset-equals-rep0 (b)", blk_nonrep, sizeof blk_nonrep, "abcdefghghgihghjihgkjihl");
    puts(ok ? "PASS: rep0 history rule" : "FAIL: rep0 history rule");
    return ok ? 0 : 1;
}
