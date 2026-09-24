# lz6 vs the field - one harness head-to-head

All numbers below come from a single harness: **lzbench 2.3.1**, one thread
(`-t1,1`), memory-to-memory per file, on an Intel Xeon E5-2697A v4 @ 2.60GHz.
lz6 and lz5 are integrated as lzbench plugins (see
[lzbench_lz6.patch](lzbench_lz6.patch)) so they are timed by the same loop,
with the same buffers and the same round-trip verification as lz4, lizard,
zstd, misa77, brotli, xz and zlib.

Last full sweep: 2026-09-24, master `9850a7b` (v1.6.5-pre: entropy-priced
optimal parser, new seq level ladder, BMI2 decode dispatch). fastlzma2 / uf-lzma2 / memlz rows are
from the 2026-09-22/23 runs on the same machine and settings. Added then, on the same
machine and settings: **fastlzma2** 1.0.1 (lzbench's build, C decoder),
**uf-lzma2** 1.1.0 (github.com/YadeWira/ultra-fast-lzma2, a fast-lzma2 fork,
with Igor Pavlov's assembler LZMA decoder: same LZMA2 stream, compressed
sizes byte-identical to fastlzma2 on every file) and **memlz** 0.2 beta.
uf-lzma2 enters through its own lzbench patch, reviewed before building.
Its level 11 (level 10 + a per-file lc/lp/pb search, standard LZMA2 output)
is **uf-lzma2 1.2.0**: measured 2026-09-23 from its v11 patch, whose sources
match the v1.2.0 tag except the version constant (checked file by file);
levels 1/5/10 were measured with 1.1.0 (level 10 gives the same bytes in
both); in that run level 10 decoded
at 120.6 MB/s on Silesia (110.5 in the 09-22 run shown), so level 11 decodes
~2% slower than level 10 when both come from the same run.

Rebuild and rerun everything with `bakeoff/run_vs_codecs.sh` (fetch + patch +
build + both corpora); the raw dumps are parsed by
`bakeoff/parse_lzbench.py`.

- **AIT A-H** (16 MB) - the training corpus this project was built against.
- **Silesia** (212 MB, 12 files) - external, never used for tuning. Both
  matter: COMPETITORS.md documents at length how AIT alone flatters lz6.

Two caveats, both material:

1. The lzbench lz6 entry is the **seq codec on one buffer per file** - exactly
   what the CLI does per file. The CLI additionally splits a single large
   mixed stream into LZ6S3 segments; on the Silesia tar that lands at 27.69%
   (L15, before seq v2) vs 26.39% here at the time, i.e. per-file separation is still the ideal and
   segmentation only recovers part of it.
2. Encode speeds for the heavy codecs are single-iteration-ish at these
   sizes; treat them as order-of-magnitude, not precise.

## AIT A-H (16 MB)

### AIT A-H (16 MB)

| codec | csize | ratio | enc MB/s | dec MB/s |
|---|---:|---:|---:|---:|
| **lz6 seq -15** | 5,095,994 | 38.79% | 1.8 | 439.5 |
| **lz6 seq -14** | 5,097,138 | 38.80% | 2.3 | 439.3 |
| **lz6 seq -13** | 5,100,233 | 38.83% | 3.4 | 441.1 |
| **lz6 seq -12** | 5,110,419 | 38.90% | 6.4 | 443.7 |
| **lz6 seq -11** | 5,113,495 | 38.93% | 9.0 | 444.3 |
| **lz6 seq -10** | 5,115,242 | 38.94% | 9.1 | 426.7 |
| **lz6 seq -9** | 5,130,458 | 39.06% | 10.0 | 424.4 |
| **lz6 seq -8** | 5,154,219 | 39.24% | 11.7 | 421.6 |
| **lz6 seq -7** | 5,220,334 | 39.74% | 21.5 | 410.9 |
| **lz6 seq -6** | 5,238,723 | 39.88% | 22.5 | 406.4 |
| **lz6 seq -5** | 5,252,196 | 39.98% | 25.9 | 403.1 |
| **lz6 seq -4** | 5,282,945 | 40.22% | 30.2 | 413.4 |
| **lz6 seq -3** | 5,373,318 | 40.90% | 77.5 | 435.3 |
| **lz6 seq -2** | 5,429,532 | 41.33% | 97.4 | 439.7 |
| **lz6 seq -1** | 5,557,735 | 42.31% | 109.1 | 449.1 |
| uf-lzma2 1.2.0 L11 (asm dec) | 6,914,570 | 52.64% | 2.2 | 64.8 |
| brotli L11 | 7,041,245 | 53.60% | 0.6 | 221.5 |
| fastlzma2 L10 | 7,067,576 | 53.80% | 6.2 | 47.0 |
| uf-lzma2 L10 (asm dec) | 7,067,576 | 53.80% | 6.2 | 63.9 |
| fastlzma2 L5 | 7,086,428 | 53.95% | 8.6 | 46.5 |
| uf-lzma2 L5 (asm dec) | 7,086,428 | 53.95% | 8.6 | 62.8 |
| xz L6 | 7,114,024 | 54.16% | 3.8 | 67.6 |
| xz L9 | 7,114,024 | 54.16% | 3.3 | 67.4 |
| fastlzma2 L1 | 7,330,324 | 55.80% | 16.3 | 49.3 |
| uf-lzma2 L1 (asm dec) | 7,330,324 | 55.80% | 16.4 | 70.4 |
| zstd L19 | 7,351,946 | 55.97% | 5.3 | 1066.4 |
| xz L3 | 7,379,360 | 56.18% | 6.7 | 66.0 |
| xz L1 | 7,435,760 | 56.60% | 8.8 | 64.2 |
| brotli L9 | 7,468,536 | 56.85% | 13.6 | 313.2 |
| brotli L6 | 7,489,339 | 57.01% | 33.3 | 313.7 |
| zstd L15 | 7,530,938 | 57.33% | 13.1 | 999.8 |
| zstd L12 | 7,552,509 | 57.49% | 35.8 | 1001.7 |
| zstd L9 | 7,578,176 | 57.69% | 67.7 | 1008.5 |
| brotli L4 | 7,591,522 | 57.79% | 80.8 | 337.0 |
| zstd L5 | 7,619,521 | 58.00% | 126.9 | 980.8 |
| zstd L3 | 7,695,386 | 58.58% | 189.4 | 1046.9 |
| zlib L9 | 7,786,253 | 59.27% | 7.1 | 310.1 |
| zlib L6 | 7,814,687 | 59.49% | 20.2 | 302.4 |
| brotli L1 | 7,855,241 | 59.80% | 261.7 | 293.5 |
| zstd L1 | 7,869,981 | 59.91% | 432.0 | 1365.2 |
| lizard L49 | 8,041,060 | 61.21% | 5.5 | 1544.1 |
| zlib L1 | 8,094,068 | 61.62% | 54.7 | 302.6 |
| lizard L45 | 8,253,375 | 62.83% | 17.7 | 1545.0 |
| lizard L40 | 8,481,600 | 64.57% | 336.8 | 1502.6 |
| lz5 HC L15 | 8,603,507 | 65.49% | 2.2 | 992.0 |
| lz5 HC L12 | 8,720,302 | 66.38% | 11.7 | 1072.4 |
| lizard L30 | 8,756,123 | 66.66% | 457.0 | 1938.6 |
| misa77 L3 | 8,867,044 | 67.50% | 10.1 | 5541.7 |
| lz5 HC L9 | 8,900,815 | 67.76% | 20.5 | 1161.4 |
| **lz6 HC -15** | 8,996,054 | 68.48% | 11.1 | 1679.4 |
| **lz6 HC -12** | 8,996,268 | 68.48% | 11.8 | 1676.6 |
| lz5 HC L6 | 9,066,986 | 69.02% | 41.4 | 1516.4 |
| misa77 L4 | 9,070,575 | 69.05% | 7.5 | 3486.3 |
| **lz6 HC -6** | 9,190,747 | 69.96% | 44.2 | 1613.1 |
| misa77 L2 | 9,287,323 | 70.70% | 38.5 | 6612.6 |
| lz5 | 9,469,498 | 72.09% | 319.9 | 1248.2 |
| misa77 L0 | 9,554,603 | 72.73% | 207.4 | 7355.4 |
| misa77 L1 | 9,628,088 | 73.29% | 51.5 | 7868.6 |
| lizard L20 | 9,761,531 | 74.31% | 457.7 | 2512.0 |
| lizard L10 | 9,926,082 | 75.56% | 634.8 | 4727.6 |
| lz4 | 9,938,605 | 75.66% | 785.7 | 5088.3 |
| misa77 L-1 | 10,041,718 | 76.44% | 327.3 | 7504.7 |
| lz5 HC L1 | 10,441,177 | 79.48% | 531.8 | 2508.5 |
| memlz | 10,724,346 | 81.64% | 1872.5 | 1712.7 |

## Silesia (212 MB, per-file)

### Silesia (212 MB, per-file)

| codec | csize | ratio | enc MB/s | dec MB/s |
|---|---:|---:|---:|---:|
| uf-lzma2 1.2.0 L11 (asm dec) | 48,483,163 | 22.88% | 1.2 | 118.3 |
| fastlzma2 L10 | 48,673,262 | 22.97% | 3.3 | 81.0 |
| uf-lzma2 L10 (asm dec) | 48,673,262 | 22.97% | 3.3 | 110.5 |
| xz L9 | 48,795,480 | 23.02% | 2.4 | 107.3 |
| brotli L11 | 50,328,370 | 23.75% | 0.5 | 351.3 |
| fastlzma2 L5 | 51,124,925 | 24.12% | 5.9 | 77.9 |
| uf-lzma2 L5 (asm dec) | 51,124,925 | 24.12% | 6.1 | 107.0 |
| zstd L19 | 52,891,946 | 24.96% | 2.6 | 735.7 |
| **lz6 seq -15** | 54,513,710 | 25.72% | 0.7 | 540.2 |
| **lz6 seq -13** | 54,632,210 | 25.78% | 1.3 | 562.8 |
| **lz6 seq -12** | 54,972,169 | 25.94% | 2.5 | 559.2 |
| **lz6 seq -10** | 55,932,487 | 26.39% | 3.8 | 560.4 |
| **lz6 seq -8** | 58,685,321 | 27.69% | 7.0 | 523.3 |
| xz L1 | 58,716,448 | 27.70% | 16.0 | 93.3 |
| fastlzma2 L1 | 58,993,508 | 27.84% | 17.8 | 65.1 |
| uf-lzma2 L1 (asm dec) | 58,993,508 | 27.84% | 18.1 | 96.4 |
| **lz6 seq -7** | 60,408,704 | 28.50% | 13.6 | 492.8 |
| lizard L49 | 60,667,327 | 28.62% | 1.8 | 1164.2 |
| zstd L5 | 62,751,501 | 29.61% | 98.5 | 774.8 |
| **lz6 seq -4** | 64,369,313 | 30.37% | 19.6 | 447.5 |
| lz5 HC L15 | 65,595,195 | 30.95% | 2.1 | 702.2 |
| **lz6 seq -2** | 67,826,536 | 32.00% | 39.3 | 469.4 |
| zlib L6 | 68,220,487 | 32.19% | 26.7 | 327.4 |
| **lz6 seq -1** | 71,030,356 | 33.51% | 40.5 | 455.7 |
| zstd L1 | 73,229,468 | 34.55% | 344.6 | 1179.6 |
| brotli L1 | 73,429,961 | 34.65% | 194.8 | 320.6 |
| zlib L1 | 77,246,811 | 36.45% | 82.2 | 301.4 |
| misa77 L3 | 80,028,169 | 37.76% | 10.5 | 4429.6 |
| lz5 HC L6 | 80,576,517 | 38.02% | 46.8 | 1160.5 |
| **lz6 HC -15** | 81,308,486 | 38.36% | 5.3 | 1277.9 |
| lizard L30 | 85,765,250 | 40.47% | 320.7 | 1224.5 |
| lz5 | 88,218,423 | 41.62% | 228.4 | 699.8 |
| misa77 L1 | 90,385,225 | 42.65% | 49.0 | 5031.5 |
| misa77 L-1 | 99,073,179 | 46.75% | 269.8 | 4679.2 |
| lz4 | 100,880,147 | 47.60% | 540.7 | 3546.8 |
| lz5 HC L1 | 113,525,877 | 53.57% | 501.8 | 1619.6 |
| memlz | 126,970,186 | 59.91% | 963.2 | 887.8 |

### Silesia per-file ratio

| file | orig | lz6 L15 | lz6 L2 | zstd -19 | xz -9 | brotli -11 | lizard -49 | lz5 HC15 | lz4 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| dickens | 9.7 MB | 28.24% | 37.47% | 27.96% | 27.77% | 27.99% | 33.00% | 36.87% | 63.07% |
| mozilla | 48.8 MB | 31.06% | 38.19% | 29.41% | 26.11% | 27.48% | 34.31% | 35.79% | 51.61% |
| mr | 9.5 MB | 33.28% | 33.88% | 31.16% | 27.58% | 28.34% | 34.20% | 37.92% | 54.57% |
| nci | 32.0 MB | 5.54% | 8.71% | 4.96% | 5.18% | 4.82% | 5.92% | 7.26% | 16.49% |
| ooffice | 5.9 MB | 44.43% | 56.98% | 42.18% | 39.45% | 40.31% | 50.21% | 49.35% | 70.53% |
| osdb | 9.6 MB | 31.05% | 34.76% | 30.74% | 28.26% | 28.01% | 33.78% | 37.30% | 52.12% |
| reymont | 6.3 MB | 20.86% | 31.32% | 20.35% | 19.87% | 20.15% | 23.34% | 27.09% | 48.00% |
| samba | 20.6 MB | 19.50% | 25.37% | 18.04% | 17.42% | 17.69% | 20.90% | 22.43% | 35.72% |
| sao | 6.9 MB | 71.58% | 79.82% | 68.95% | 60.88% | 63.29% | 73.89% | 78.09% | 93.63% |
| webster | 39.5 MB | 20.64% | 29.54% | 20.93% | 20.23% | 21.20% | 24.64% | 27.00% | 48.58% |
| x-ray | 8.1 MB | 57.02% | 57.02% | 60.53% | 52.98% | 55.30% | 66.91% | 75.46% | 99.01% |
| xml | 5.1 MB | 9.48% | 13.30% | 8.47% | 8.48% | 8.05% | 9.98% | 11.23% | 22.96% |

## Findings

**AIT: the lead is file D.** L15 is 38.79% on the 8 files against brotli
-11's 53.60% and zstd -19's 55.97%, decoding at 440 MB/s. Almost all of that
comes from one file: D is glibc `random()` output, which lz6 regenerates
from its seed (2,000,000 -> 7 bytes) while every other codec stores it raw.
Without D, lz6 -15 is 45.76%: ahead of xz -9 and zstd -19, behind brotli
-11 and LZMA2 (uf-lzma2 -10/-11):

| codec | 8 files | without D | A | B | C | E | F | G | H |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **lz6 -15** | **38.79%** | 45.76% | 54.72% | 18.41% | 27.07% | 79.53% | 79.43% | 29.36% | 40.17% |
| brotli -11 | 53.60% | 45.27% | 50.85% | 17.41% | 24.92% | 84.75% | 79.28% | 30.97% | 36.26% |
| uf-lzma2 -10 | 53.80% | 45.50% | 51.87% | 17.78% | 24.78% | 85.52% | 82.53% | 28.53% | 35.97% |
| xz -9 | 54.16% | 45.92% | 53.34% | 17.76% | 24.72% | 85.84% | 82.38% | 29.71% | 35.87% |
| zstd -19 | 55.97% | 48.06% | 51.77% | 18.06% | 26.66% | 88.52% | 87.83% | 31.61% | 38.49% |
| uf-lzma2 -11 | 52.64% | 44.13% | 51.87% | 17.78% | 24.59% | 78.01% | 79.12% | 28.51% | 35.86% |

Per file, lz6 -15 has the best E among the codecs without a per-file
parameter search (79.53%, uf-lzma2 -11: 78.01%); on F it is within 0.3
points of brotli -11 and uf-lzma2 -11; on A, B, C and H it still trails
zstd -19 (by 3.0, 0.4, 0.4 and 1.7 points), a gap the entropy-priced
parser narrowed from 6.0, 1.2, 2.2 and 2.5. (Until 2026-09-22 these
tables showed lz6 at 46.66% against the other codecs' 8-file totals: the
parser rebuilt each file's size from lzbench's rounded ratio, and D's 0.00
ratio dropped it from lz6's denominator only. Fixed in parse_lzbench.py,
which now uses the real file sizes; reported by uf-lzma2.)

**On Silesia lz6 -15 is 0.8 points behind zstd -19.** L15 is 25.72% @ 540
MB/s versus zstd -19's 24.96% @ 736 MB/s and brotli -11's 23.75% @ 351
MB/s. The entropy-priced parser (v1.6.5-pre) halved the ratio gap (it was
1.56 points) and the decode lead of zstd -19 is down to 1.36x (2.0x before
v1.6.4-pre). lizard -49 is 2.9 points behind lz6 at 2.2x the decode.

**LZMA2 with an assembler decoder.** uf-lzma2 -11 is the best ratio in the
Silesia table (22.88%; -10: 22.97%, xz -9: 23.02%) at ~110-118 MB/s decode;
its -10 streams decode 36-48% faster than the same streams through
fastlzma2's C decoder. That is the ceiling of the bit-wise range-coder
class; lz6 -15 trades 2.8 points of ratio for ~4.6x the decode. Level 11's
per-file lc/lp/pb search also takes AIT/E from 85.52% to 78.01% (lz6's
byte-plane result there: 79.53%).

**memlz** is the opposite end: ~0.9 GB/s both ways on Silesia (1.7-1.9 GB/s
on AIT) at 59.91%, a worse ratio than lz4 (47.60%): a compression-speed codec.

**Per file, lz6 L15 now wins x-ray and webster** (57.02% vs 60.53%, 20.64%
vs 20.93%) and is within 0.3 points of zstd -19 on dickens (28.24 vs 27.96)
and osdb (31.05 vs 30.74). The remaining losses are the binary/structured
files: sao (71.58 vs 68.95), ooffice (44.43 vs 42.18), mr (33.28 vs 31.16),
mozilla (31.06 vs 29.41) and samba (19.50 vs 18.04).

**The entropy-priced parser (v1.6.5-pre).** The seq engine used to reuse the
frame codec's optimal parser, which prices a literal at 8 bits and an offset
at 1-4 codeword bytes. It now prices with -log2 frequencies of the ll/ml/of
symbols plus extra bits and the literals' order-0 entropy, from a cheap
pre-parse: Silesia L15 26.52% -> 25.72%, and decode is faster too (fewer
sequences). The same prices in the heuristic parsers of levels 4-7 made
them worse and were rejected.

**The decoder rewrite (v1.6.4-pre).** Same harness, same machine, lz6 seq
decode before -> after: Silesia L15 274 -> 399 MB/s (+46%), L9 239 -> 356,
L2 255 -> 364, L1 260 -> 372; AIT L15 234 -> 291 (+25%), L2 278 -> 359.
Literals are decoded in bulk (two-symbol Huffman lookups), the rANS renorm
is branch-free with L1-resident 4-byte table entries, and a two-stage
sequence loop prefetches match sources 16 sequences ahead. Sizes moved by
+0.01-0.04% from the 12-bit rANS precision cap. Details: the wiki's
Decoder Internals page.

**Seq v2 (v1.6.4-pre).** The ll/ml/of symbols moved from three rANS streams
to table ANS sharing one backward bitstream with all extra bits (offsets as
bucket + raw bits). Same harness, before -> after: Silesia L15 399 -> 493
MB/s (+23%), L9 356 -> 451 (+27%), L2 364 -> 446 (+23%); AIT L15 320 -> 405
(+26%), L2 389 -> 436. Size: Silesia L15 26.39% -> 26.52%, L2 31.92% ->
32.00% (the dropped per-bucket offset model); AIT -0.04%.

**The repcode merge - why it worked.** Instrumenting the encoder showed
offsets dominate: 42-82% of the output (dickens L15: 2.75 MB of 3.36 MB),
with only 12% of sequences using a repcode - yet every sequence paid a fixed
2-bit rep field. Folding the repcode into the offset FSE alphabet codes the
joint (bucket, repcode) distribution instead, saving ~1.4 bits per sequence
on text (measured: Silesia tar L15 62,437,010 -> 58,693,799, -6.0%; AIT
5,367,989 -> 5,194,253, -3.2%) and removing a bitfield read, so decode got
~6% faster as well. It also breaks the seq block format - see NEWS.

**Decode speed remains lz6's weak axis vs zstd/lizard.** lz6's 420-560
MB/s band sits 1.3-3x below zstd (736-1340) and lizard (1160-1900), and
~9-18x below misa77 (4600-7800, at much worse ratio). The seq v2 decoder is
instruction-bound (IPC ~2.5); see the wiki's Decoder Internals and Roadmap
pages.

**Level ladder (v1.6.5-pre).** The seq levels have their own table and form
a strictly monotonic ladder: Silesia L1 33.51% -> L15 25.72%, every level
smaller and slower than the one before (the old table had byte-identical
pairs). L13-L15 differ only by refinement re-parses, so they sit close
together (25.78 / 25.74 / 25.72%); deeper searches gave nothing past 64.

**Not benchmarked here:** brotli/xz are different families with no
fast-decode pretension. The uncomfortable line is still zstd -19, smaller
and 1.36x faster to decode on Silesia; the remaining ratio gap is in the
binary/structured files above.
