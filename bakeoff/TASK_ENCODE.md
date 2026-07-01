# Task: speed up the lz6 high-compression ENCODE hot loop

You are optimizing **encode (compression) throughput** of the lz6 compressor (a
byte-oriented LZ77 codec, fork of LZ5 v1.5). Repo builds with `make -C programs` →
`programs/lz6`. This task targets the **high-compression / optimal-parser levels
(11–15)**, which are the slow ones (~1–9 MB/s).

## Goal
Make the high-compression encoder **faster** (MB/s) **without losing compression ratio**
and **without changing round-trip correctness**. Almost all the time is in one function.

## Where the time actually goes (measured — callgrind + cachegrind, level 14)
- **~90%** of instructions are in `LZ6HC_compress_optimal_price` (`lib/lz6hc.c:1051`).
- Inside it, the dominant cost is the **hash-chain match search** in
  `LZ6HC_GetAllMatches` (`lib/lz6hc.c:674`), which the parser calls at *every* position
  and which walks up to `searchNum` chain links (128 at L14, 256 at L15). Line-level:
  - ~23% — the chain-walk loop condition (`lib/lz6hc.c:739`)
  - ~11% — the chain hop `matchIndex -= chainTable[matchIndex & contentMask]` (`:797`)
  - ~10% — the candidate head-check `ip[best_mlen]==match[best_mlen] && MEM_read24(...)`
  - ~17% — the `matchIndex >= dictLimit` branch + `match = base + matchIndex`
  - ~4%  — `MEM_count` match-length extension + backward extension
- **The real wall-clock bottleneck is the serial load-dependency chain**, not raw
  instruction count. `matchIndex -= chainTable[matchIndex & contentMask]` feeds the
  *next* iteration's address from *this* iteration's load, so the hops cannot overlap —
  each waits on the previous load's latency. D1 read-miss rate is ~8.6% but last-level
  miss rate is ~0.02% (working set lives in L2/L3), so each stalled hop costs ~L2 latency
  and there are up to 128–256 of them per position, in series.
- **Measured dead-end (don't repeat it):** the `matchIndex >= dictLimit` branch is ~17%
  of *instructions* but it is almost-always-true → perfectly predicted → specializing it
  out of the loop gave **+0.0%** wall-clock in an A/B test at byte-identical output.
  Callgrind's instruction% badly overstates the cost of predictable branches here. Chase
  the load-latency chain and the number of hops, not the instruction count.

## Hard rules (violating any = DISQUALIFIED)
1. **Round-trip must stay correct.** Every file that compresses must decompress to a
   byte-identical copy, at every level. The harness checks this first.
2. **Ratio must NOT regress.** The output is not fixed for an encoder: you can "go
   faster" by finding fewer/shorter matches, which silently costs ratio. The harness
   compares the compressed output **byte-for-byte against the recorded baseline**.
   - **Best outcome: byte-identical output** — same matches/prices chosen, just computed
     faster. Aim for this. A pure-speed optimization does not change what the parser picks.
   - Output larger than baseline by more than 0.10% → DISQUALIFIED.
3. **Safe on all inputs (ASAN).** The match finder does pointer arithmetic on `U32`
   indices. **Do not remove or reorder the hot-loop guard conditions without proving
   they are not load-bearing.** Concretely: `matchIndex < current` in the `:739` loop
   condition is a **wraparound guard** — `matchIndex -= chainTable[...]` is unsigned, and
   if a chain delta exceeds `matchIndex` it wraps to a huge value; the `< current` test
   catches that and exits before `base + matchIndex` reads out of bounds. Strip it and
   you get an OOB read on some inputs even though round-trip on your test file passes.
   The harness runs an **AddressSanitizer build + round-trip**; any OOB = DISQUALIFIED.
4. **Do not change the block format** (`lz6_Block_format.md`) or the decoder. Touch only
   the encode path — realistically `LZ6HC_GetAllMatches` and `LZ6HC_compress_optimal_price`
   in `lib/lz6hc.c`, plus small helpers.
5. **Stay portable.** Must compile and run correctly with a plain scalar path. Any SIMD
   (SSE2/AVX2/NEON) goes behind `#ifdef __SSE2__` / `__AVX2__` / `__ARM_NEON` with an
   equivalent scalar fallback — never the only path.
6. Keep the diff focused and readable.

## Where the speed is (ideas, not a checklist — every one must stay ratio-neutral)
- **Attack the load-latency chain (the real bottleneck).** The chain hop is a serial
  pointer-chase. Ideas that can hide/reduce that latency while examining the *same*
  candidates and picking the *same* match: software-prefetching the match bytes
  (`base + matchIndex`) and/or the next chain slot as early as the index is known;
  restructuring so an independent computation overlaps the load; reducing the *number* of
  hops that are actually productive without dropping any match the baseline would keep
  (e.g. a cheaper early-reject that provably cannot discard a winning candidate).
  NOTE: naively specializing the `matchIndex >= dictLimit` branch out was measured at
  **+0.0%** — predictable branches are already free; don't spend the diff there.
- **Tighten the per-hop work only where it isn't a predicted branch.** Reordering the
  short-circuit conditions is fine *only if* it preserves the wraparound guard (rule 3)
  and examines the same candidates. Don't expect much from instruction-count trimming
  alone — the loads dominate.
- **`MEM_count` (match-length compare)** and the backward-extension `while` loop (`:751`)
  are byte/word loops — SIMD or wider word compares behind `#ifdef` can help, but the
  computed match length must be identical to the scalar result (off-by-one here changes
  output and ratio).
- **The head-check filter** `ip[best_mlen] == match[best_mlen]` already skips candidates
  that can't beat the current best; keep it, don't weaken it.
- Do **not** switch match-finder strategy (e.g. to the binary-tree finder) or change
  `searchNum`, `sufficientLength`, hash sizes, or the price model — those change which
  matches are chosen and therefore the ratio. This task is pure speed at equal output.

## Reference material in the repo
- `lib/lz6hc.c` — the HC encoder. Key spots: `LZ6HC_GetAllMatches` (`:674`),
  `LZ6HC_compress_optimal_price` (`:1051`), the param table is in `lib/lz6common.h`
  (`LZ6HC_defaultParameters`, levels 11–15 use `LZ6HC_optimal_price`).
- `lib/mem.h` — `MEM_count` / `MEM_read24/32` / `MEM_NbCommonBytes` helpers.
- `lz6_Block_format.md` — bitstream format (do not change it).

## How you are judged (run this yourself and iterate to the number)
```
bakeoff/bench_encode.sh <your-label>
```
It: builds under **ASAN** and round-trips (safety gate) → rebuilds `-O3` → round-trip gate
on 3 corpora at levels 1/6/11/15 → **ratio gate** (compressed output vs baseline,
byte-compared) → prints **encode MB/s + Δ%** vs baseline at levels 11/14/15. Accepted only if:
- round-trip passes (hard gate), AND
- the ASAN gate passes — no OOB (hard gate), AND
- ratio does not regress — ideally byte-identical output (hard gate), AND
- encode MB/s goes **up** vs baseline, AND
- it still builds/works with SIMD disabled.

Baseline numbers are in `bakeoff/ENCODE_BASELINE.txt`; baseline compressed outputs are in
`bakeoff/encode_baseline/`. Beat the MB/s without touching the output bytes. Report your
final encode MB/s per corpus/level and the diff.
