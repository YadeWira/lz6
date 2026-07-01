# Task: speed up the lz6 decompression hot loop

You are optimizing **decode throughput** of the lz6 compressor (a byte-oriented LZ77
codec, fork of LZ5 v1.5). Repo builds with `make -C programs` → `programs/lz6`.

## Goal
Make `LZ6_decompress_generic` in `lib/lz6.c` (starts ~line 896) decode **faster**,
measured in MB/s, **without changing the bitstream format or the encoder output**.

## Hard rules (violating any = failure)
1. **Round-trip must stay correct.** Every file that compresses must decompress to a
   byte-identical copy. A faster-but-wrong decoder is disqualified.
2. **Safe on malformed input.** This is a decompressor — it processes untrusted data.
   It must never read or write out of bounds on ANY input, valid or corrupted. In
   particular: **do not widen a copy that runs before its bounds check.** The classic
   trap is copying 16/32 bytes in the match-copy fast path *before* the
   `cpy > oend-...` guard — that overflows the output buffer on a crafted stream even
   though round-trip on valid files still passes. The harness runs an ASAN fuzz gate
   (`bakeoff/fuzz_decode.c`) that decodes corrupted blocks into an exactly-sized buffer;
   any out-of-bounds access = DISQUALIFIED. Every copy must be bounded by a check that
   accounts for its full width.
3. **Do not change the block format** (`lz6_Block_format.md`) or the encoder. Touch only
   the decode path (`LZ6_decompress_generic` and helpers it calls).
4. **Stay portable.** Must compile and run correctly with a plain scalar path. Any SIMD
   (SSE2/AVX2/NEON) goes behind `#ifdef __SSE2__` / `__AVX2__` / `__ARM_NEON` with an
   equivalent scalar fallback — never the only path.
5. Keep the diff focused and readable. Prefer `lib/lz6.c` only.

## Where the speed is (ideas, not a checklist)
- The literal-copy and match-copy loops: overlapping/`wildcopy`-style copies, removing
  branches from the hot path, widening copies to 8/16/32 bytes with a safe tail.
- Reducing per-token work: the offset/length decode, the flag dispatch (4 codewords:
  `1`=10-bit off, `00`=16-bit, `010`=24-bit, `011`=repeat-offset/no offset bytes).
- Instruction-level parallelism, avoiding data-dependent branches, better bounds checks.
- Match copy with small offsets (overlap) is the classic LZ decode bottleneck — handle
  offset<8 / offset<16 without byte-at-a-time loops where safe.

## Reference material in the repo
- `lib/lz6.c` — the decoder (and encoder fast path).
- `lz6_Block_format.md` — exact bitstream: tokens, the 4 offset codewords, minmatch 3,
  the "last 5 bytes are literals" and "last match ≥12 bytes from end" parsing rules.
- The repeat-offset codeword (flag `011`) reuses the previous offset and emits no offset
  bytes — the decoder tracks `last_off`.

## How you are judged (run this yourself and iterate to the number)
```
bakeoff/bench_decode.sh <your-label>
```
It rebuilds, runs the round-trip gate on 3 corpora at levels 1/6/11/15, then the
**ASAN malformed-input fuzz gate**, then prints decode MB/s. Your change is accepted
only if:
- round-trip passes (hard gate), AND
- the ASAN fuzz gate passes — no out-of-bounds on corrupted input (hard gate), AND
- decode MB/s goes **up** vs baseline (the pre-change number), AND
- it still builds/works with SIMD disabled.

Baseline decode numbers are recorded in `bakeoff/BASELINE.txt`. Beat them without
breaking round-trip. Report your final decode MB/s per corpus and the diff.
