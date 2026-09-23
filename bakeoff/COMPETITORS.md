# Competitive analysis: zstd, lizard, misa77 (vs lz6)

> **Update 2026-09-13:** the complete head-to-head comparison, with every
> codec in a single harness (lzbench) and Silesia as well as AIT, is in
> **[VS_CODECS.md](VS_CODECS.md)**. It includes lz5 v1.5 and lz6 as lzbench
> plugins. This document remains the original analysis on AIT; its lz6
> figures predate the hash widening, the plane-gate fix and the v1.6.4-pre
> decoder rewrite (current decode numbers: VS_CODECS.md).

Corpus: AIT A-H (13,136,308 bytes), single-threaded, mem-to-mem measurement.
Externals via lzbench 1.2 (`-t2,2`); lz6 seq via `bakeoff/bench_seq.c` (same
mem-to-mem methodology, MB/s corrected to output bytes). Date: 2026-08-28,
lz6 rev: post f638c16. gzip -6 measured separately as the Weissman anchor.

## Aggregate table (sorted by ratio)

| codec | csize | ratio | enc MB/s | dec MB/s |
|---|---:|---:|---:|---:|
| **lz6 seq L15** | **5,361,732** | **40.82%** | ~15 | ~222 |
| **lz6 seq L2 (default)** | **5,596,570** | **42.60%** | ~120 | ~262 |
| zstd 1.5.7 -9 | 7,578,176 | 57.69% | 61 | 985 |
| zstd 1.5.7 -3 | 7,695,386 | 58.58% | 183 | 1,033 |
| zstd 1.5.7 -1 | 7,869,981 | 59.91% | 413 | 1,322 |
| gzip -6 (anchor) | 7,820,934 | 59.53% | 15 | ~110-400 |
| lizard 2.1 -45 | 8,253,375 | 62.83% | 15 | 1,490 |
| **lz6 --hc -15 (frame)** | 8,588,303 | 65.38% | 5 | 1,016 |
| lizard 2.1 -30 | 8,756,123 | 66.66% | 428 | 1,865 |
| misa77 0.6.0 -4 | 9,070,575 | 69.05% | 6 | 3,593 |
| misa77 0.6.0 -2 | 9,287,323 | 70.70% | 36 | 6,519 |
| misa77 0.6.0 -1 | 9,628,088 | 73.29% | 49 | 7,761 |
| lizard 2.1 -20 | 9,761,531 | 74.31% | 429 | 2,398 |
| lizard 2.1 -10 | 9,926,082 | 75.56% | 597 | 4,640 |
| lz4 1.10.0 | 9,938,605 | 75.66% | 739 | 4,985 |
| misa77 0.6.0 --1 | 10,041,718 | 76.44% | 309 | 7,384 |

## Reading, competitor by competitor

### misa77 (the decode threat)

The project's thesis is "write-once, read-many": memcpy-class decode at the
cost of slow encode. On the AIT aggregate it decodes at 7.7 GB/s (L1) with
73.29%: **30x faster than our seq, 31 points worse ratio**.

Where it beats us (text):
- C: misa -1 = 42.31% @ 5,436 MB/s vs lz6 seq L2 = 37.71% @ 180 (-4.6 pts ratio, 30x decode)
- B: misa -1 = 31.48% @ 6,366 vs lz6 29.85% @ 232 (-1.6 pts, 27x)
- H: misa -1 = 57.46% @ 5,155 vs lz6 57.40% @ 179 (**ratio tie, 29x decode**)

Where it fails (our structural strengths):
- A (high-entropy binary): misa -1 = 99.47% (does not compress); lz6 seq = 53.14%
- E: misa = 100.00%; lz6 = 79.76% (plane transform)
- F: misa = 100.00%; lz6 = 79.29%
- D (PRNG): misa = 100.00%; lz6 = 0.00% (regenerated from the seed, 7 bytes)

Root cause: misa77's light format has no entropy coding of literals (pure LZ
with a high-bandwidth encoding). Its whole decode is a copy loop with no
FSE/rANS table. Our ratio comes from exactly what slows our decode down.

Weissman verdict (AIT, ratio×speed vs gzip 59.53%): misa77 -1 has
ratio_factor = 59.53/73.29 = 0.81 (< 1: worse than gzip on ratio) but a huge
speed_factor. On pure text files the combination can beat us; on the AIT
aggregate our ratio_factor of 1.40 with decode at 2-3x gzip holds the
challenge's #1 spot. The real leaderboard decides: if misa77 entered the
challenge competing per file, it would be a direct rival on B/C/H.

### zstd (the generalist)

Still #2 on ratio (57.69% at -9) and the best overall balance. It decodes
3.8x faster than our seq with a 15-point worse ratio. Its -1..-3 range
(58.6-59.9% @ 1.0-1.3 GB/s dec) is the natural commercial comparison point.
On A it has no rival among the externals at low speed (52.41% at -1, better
than our 53.14%).

### lizard (the ancestor)

A fork of lz5 with entropy modes: -30 (FSE) = 66.66% @ 1,865 dec; -45
(maximum) = 62.83% @ 1,490. Our seq beats it by 20+ points of ratio in the
same encode class. Empirical confirmation that the lz5+lizard line stopped
halfway to what seq already implements. Its value today is historical: look
at what lizard -30 did (FSE literals) and why it fell short of our full
pipeline.

### lz4

The pure-speed reference. Our default frame (fast, ~76% @ 739/4,985) is in
its class; our seq beats it by 33 points of ratio with 7x slower decode.

## Opportunities (ordered by expected impact)

1. **Seq decode is THE gap**: 262 MB/s vs 1-8 GB/s for the competition.
   Levers already identified (see the Weissman plan): streaming decode
   (do not materialise arrays), wildcopy in copies, batched refills, and the
   misa77-style idea of maximising copy width per sequence. Every MB/s of
   decode raises the Weissman score directly.
2. **LZ6S2 hybrid format**: seq blocks for ratio + light-style blocks (pure
   LZ, memcpy-class decode) for blocks the parser marks as "easy"; the
   decoder would pick a path per block. The frame already supports per-block
   codecs; only an encoder-side selector is missing.
3. **Smarter raw literals**: A/H/E/F show that when literals do not compress
   we pay the minimum cost, while misa77 does not even try. Our wins there
   (plane, PRNG) are hacker transforms; more of them (delta for structured
   binary, RGB/float lanes) widens the gap.
4. **Do not chase misa77's decode at the cost of ratio**: its advantage comes
   from having NO entropy stage; copying its design would lose us the 15
   points of ratio over zstd, which is our only defensible crown.

## Methodology and caveat

- lzbench measures mem-to-mem without I/O; so does bench_seq (decode MB/s
  corrected by output size). Numbers across harnesses are comparable in
  order of magnitude, not to the digit.
- Decode noise on this machine is ±5% (see SEQ_SPEED_BASELINE.txt); use the
  D/E canaries for fine comparisons.
- misa77 0.6.0 (13 commits, v0.x): unstable format, no safe decoder for its
  level 4. High adoption risk for third parties, but the decode engineering
  is real and its presence in lzbench/TurboBench gives it visibility.


---

## Round 2 (post LZ6S2): Silesia.tar, the mirror that exposes the AIT bias

Corpus: Silesia.tar (211,957,760 bytes, 12 heterogeneous files). Same
methodology; gzip -6 as the anchor (25 enc / ~174 dec MB/s).

| codec | csize | ratio | enc MB/s | dec MB/s |
|---|---:|---:|---:|---:|
| zstd -9 | 59,071,826 | 27.87% | 53 | 656 |
| **lz6 seq L15** | **62,437,010** | **29.46%** | **2.2** | **141** |
| lizard -45 | 66,676,865 | 31.46% | 18.8 | 1,078 |
| zstd -3 | 66,133,605 | 31.20% | 158 | 702 |
| **lz6 --hc -15 (frame)** | **65,237,073** | **30.78%** | **2.7** | **994** |
| gzip -6 (anchor) | 68,235,411 | 32.19% | 25 | ~174 |
| zstd -1 | 73,193,861 | 34.53% | 348 | 1,135 |
| misa77 -4 | 75,259,843 | 35.51% | 6.6 | 1,028 |
| **lz6 seq L2** | **90,730,094** | **42.81%** | **99** | **272** |
| misa77 -1 | 90,386,470 | 42.64% | 49.3 | 4,131 |
| lz4 | 100,881,076 | 47.59% | 512 | 3,244 |
| lizard -10 | 103,401,614 | 48.78% | 433 | 2,984 |

### Findings that change the roadmap

1. **The ratio crown is AIT-specific.** On AIT our L2 (42.61%) crushes
   zstd -1 (59.91%); on Silesia zstd -1 (34.53%) runs over us by 8 points
   and decodes 4x faster. The gap concentrates in the large binaries
   (mozilla 51MB, nci): zstd's generic literal coding and match strategy vs
   our reliance on specific transforms (plane/PRNG) that do not cover this
   data.

2. **Hash table saturation at scale.** With 100MB+ of input and 8M buckets
   (L2), the chains saturate and the shallow search (searchNum=2) grabs
   recent, low-quality candidates. Measured: on the 100MB slice, 16MB blocks
   (short chains) compress 4.5 points BETTER than 64/100MB blocks with the
   same 32MB window. On mozilla-type data the effect dominates.

3. **The CLI's B6 default was the worst point for seq on Silesia** (48.58%
   vs 42.81% for B7: 12MB of difference from boundary dead zones against the
   32MB window). The seq default is now B7 (single block up to 256MB,
   ~3.5GB RAM); AIT unchanged (same single block).

4. **The HC frame (L15) is competitive on Silesia** (30.78% @ 994 MB/s dec,
   7x our seq L15 with a 1.3-point worse ratio): the frame's LZ decoder with
   wildcopy is still the project's fast-decode engine.

### Revised roadmap (by evidence)

1. **Literal coding for binaries**: the Silesia gap lives in mozilla/nci:
   FSE literals with binary contexts (zstd-style offsets/extended contexts)
   or a generalised order-1 mode with lazy tables.
2. **Matcher at scale**: searchNum adaptive to chain saturation, or a wider
   hash at low levels; recovers ratio on large files without touching encode
   speed on small ones.
3. **Challenge Weissman**: an AIT rerun with the final build gave L2
   5,597,278 / L15 5,362,556, unchanged vs what was recorded.

### Round 3: the plane-gate bug on text (2cb604a)

The plane's "decisive" gate compared against 80% of the input instead of
against the normal block, and it SKIPPED the normal pass. The skew gate
(min lane < 5 b/B) passes on TEXT (the stride-2 lanes of a novel: 4-4.5
b/B; it was tuned only for the E/F/G floats). Result: dickens/reymont/
mozilla were compressed through the plane path with lanes at a hardcoded L1,
and order-1 literal coding never ran (L2 = L15 byte-identical).

Fix: skip only on extreme skew (min lane <= 2 b/B: float exponent planes);
everything else runs the normal pass and compares.

Per-file CLI after the fix: L15 aggregate 29.46% -> 27.91% (tied with
zstd -9, 0.04 points behind), L2 37.05% (zstd -1: 34.53%). dickens
57.16 -> 32.95, reymont 52.03 -> 24.20 (beats zstd -9), mozilla
53.52 -> 33.83. Floats (mr/sao/x-ray) untouched.

### LZ6S3 implemented (content segmentation)

The CLI compression loop reads 256KB windows and cuts a block when the
window's H0 entropy diverges by >= 1.0 b/B from the open segment's profile
(minimum segment 4MB). Each block is a full LZ6_compress_seq with its own
literal mode and transform: per-file adaptation inside one frame.

Silesia.tar CLI results: L2 42.81% -> 36.87%, L15 29.46% -> 28.34%
(the per-file sum, 27.91%, is the ceiling: the detector does not cut every
boundary). AIT byte-identical. The thresholds are insensitive in 0.6-1.5.

**Documented negative result**: a dual detector adding per-window H1
(order-1) was tried and REJECTED. Per-window H1 varies inside mozilla as
much as across file boundaries, so the spurious cuts cost more than they
detect (L2 +30KB, L15 +153KB).

### Round 5: the repcode was the 6% (and two refuted hypotheses)

Instrumenting the encoder (`LZ6_SEQ_STATS=1`) shows the real byte
breakdown: **offsets are 42-82% of the output** (dickens L15: 2.75M of
3.36M), not literals (2% on text). And the repcode was coded as a FIXED
2-bit field per sequence, when only 12% of sequences use it on text and its
real entropy is 0.56 bits. Folding it into the offset alphabet (zstd style):
**Silesia tar L15 -6.0%, AIT -3.2%, decode +6%** (commit dc388d6).

With the instrument in hand, two more hypotheses were tested, both refuted:

**H1: the order-1 literal gate is mozilla's limit.**
REFUTED. Forcing the gate (0.90 -> 2.00, i.e. always try order-1) does not
change mozilla's size at all, and the entropies the gate reports explain
why: h0=7.974 (almost uniform), h1=7.883 with a nibble context = 0.989, i.e.
1.1% in theory against an 8KB header. mozilla's literals are genuinely
incompressible for a byte-context model; order-1 is not the lever.

**H2: the 32MB window lets the parser pick far offsets that do not pay.**
REFUTED. Sweep of a hard distance cap in the price model
(256K/1M/4M/16M/unlimited), L15, one-shot size:

| cap | mozilla | sao | dickens | webster | nci |
|---|---:|---:|---:|---:|---:|
| 256K | 17,007,043 | 5,167,421 | 3,346,439 | 10,504,657 | 1,990,686 |
| 1M | 16,698,163 | **5,154,594** | 3,174,931 | 9,858,644 | 1,969,856 |
| 4M | 16,442,967 | 5,174,862 | 3,088,519 | 9,375,991 | 1,953,339 |
| 16M | 16,293,854 | 5,182,271 | 3,076,668 | 9,051,936 | 1,943,081 |
| unlimited | **16,259,654** | 5,182,271 | **3,076,668** | **8,994,073** | **1,941,720** |

Far offsets pay on every file except **sao**, which improves 0.53% with a
1MB cap (the only case where a ceiling is justified). The large window is
justified; mozilla's gap (31.74% vs zstd 29.41%) does not come from there.

**H3: LZMA-style matched literals (delta against the rep0 prediction).**
REFUTED, and the methodological lesson is worth more than the result. The
full mode was implemented (encoder + decoder + mode 7 in the seq block):
each literal is delta-XORed against the byte at (pos - rep0), which the
decoder already has, and the candidate competes with Huffman/FSE/order-1.
Measured gains: mozilla -0.64%, ooffice -0.75%, sao -0.29%, text 0% ->
**-0.37% aggregate**, not enough for a new format mode, so it was reverted.

Why the earlier estimate said -3.8%: the diagnostic measured h0 over the
literals and compared it with the h0 reported by the order-1 gate... which
is computed on a **64K sample**, not the block. On mozilla that sample gives
h0=7.974 (almost uniform), but the real entropy of the whole block is ~7.00
bits, which order-0 Huffman already reaches. The delta stream measures 7.277
bits: it is WORSE. Per block: raw hsz=6,537,671 vs delta hsz=6,817,564.

Conclusion: byte-exact prediction from rep0 does not beat order-0 on this
data. What LZMA does differently is a **bit-wise** model guided by the match
byte (it captures partial agreement, not only equality); that would be a new
literal coder, not one more mode.

### Open investigation: single block vs per file (~5%)

The tar as ONE L15 block = 62.4M vs the per-file sum = 59.1M.
Instrumentation (LZ6_SEQ_VERBOSE=1): in the tar the plane is rejected (mixed
lanes >= 5 b/B) and a single literal mode (o1-256, which beat Huffman 16.79
vs 16.94 MB) serves 17M mixed literals; per file, each adapts (o1 on text,
plane on sao ~0.75M, Huffman where it wins). The natural fix is content
segmentation (LZ6S3): detect entropy transitions and split the inner block
by region, with its own literal mode per segment.
