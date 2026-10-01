# lz6 vs the field - one harness head-to-head

All numbers below come from a single harness: **lzbench 2.3.1**, one thread
(`-t1,1`), memory-to-memory per file, on an Intel Xeon E5-2697A v4 @ 2.60GHz.
lz6 and lz5 are integrated as lzbench plugins (see
[lzbench_lz6.patch](lzbench_lz6.patch)) so they are timed by the same loop,
with the same buffers and the same round-trip verification as lz4, lizard,
zstd, misa77, brotli, xz and zlib.

Last full sweep: 2026-10-01, v1.6.9-pre (x86 branch filter, 32-slot row hash
at levels 5-6, byte-plane lanes tried at level 4), run pinned to one core
(`taskset -c 40`): a first unpinned run on the shared machine came out
10-25% slow and noisy across all codecs and was discarded. fastlzma2 / uf-lzma2 / memlz rows are
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
~2% slower than level 10 when both come from the same run. uf-lzma2 1.3.0 and
1.4.x / 1.5.x were not re-measured: per its author, nothing changed since 1.2.0 on the
path lzbench uses (native one-shot UF2_compressMt / UF2_decompressMt; later
releases add .xz, streaming and hardware CRC), and compressed sizes still
match fastlzma2 at levels 1/6/10.

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
| **lz6 seq -15** | 5,084,398 | 38.70% | 1.7 | 572.0 |
| **lz6 seq -14** | 5,085,566 | 38.71% | 2.3 | 578.3 |
| **lz6 seq -13** | 5,089,348 | 38.74% | 3.3 | 576.1 |
| **lz6 seq -12** | 5,099,348 | 38.82% | 6.2 | 584.8 |
| **lz6 seq -11** | 5,102,491 | 38.84% | 8.2 | 582.9 |
| **lz6 seq -10** | 5,104,233 | 38.86% | 8.4 | 587.1 |
| **lz6 seq -9** | 5,119,651 | 38.97% | 9.1 | 577.7 |
| **lz6 seq -8** | 5,140,591 | 39.13% | 10.6 | 561.8 |
| **lz6 seq -6** | 5,174,748 | 39.39% | 43.5 | 611.5 |
| **lz6 seq -5** | 5,175,781 | 39.40% | 44.5 | 610.4 |
| **lz6 seq -7** | 5,177,633 | 39.41% | 20.9 | 580.7 |
| **lz6 seq -4** | 5,188,568 | 39.50% | 49.2 | 607.1 |
| **lz6 seq -3** | 5,219,589 | 39.73% | 94.0 | 583.0 |
| **lz6 seq -2** | 5,294,924 | 40.31% | 182.0 | 630.7 |
| **lz6 seq -1** | 5,335,737 | 40.62% | 212.7 | 667.8 |
| uf-lzma2 L11 | 6,914,570 | 52.64% | 2.2 | 64.8 |
| brotli L11 | 7,041,245 | 53.60% | 0.6 | 213.3 |
| fastlzma2 L10 | 7,067,576 | 53.80% | 6.2 | 47.0 |
| uf-lzma2 L10 | 7,067,576 | 53.80% | 6.2 | 63.9 |
| fastlzma2 L5 | 7,086,428 | 53.95% | 8.6 | 46.5 |
| uf-lzma2 L5 | 7,086,428 | 53.95% | 8.6 | 62.8 |
| xz L6 | 7,114,024 | 54.16% | 3.8 | 65.7 |
| xz L9 | 7,114,024 | 54.16% | 3.1 | 65.7 |
| fastlzma2 L1 | 7,330,324 | 55.80% | 16.3 | 49.3 |
| uf-lzma2 L1 | 7,330,324 | 55.80% | 16.4 | 70.4 |
| zstd L19 | 7,351,946 | 55.97% | 4.6 | 1048.7 |
| xz L3 | 7,379,360 | 56.18% | 6.8 | 63.7 |
| xz L1 | 7,435,760 | 56.60% | 9.7 | 61.9 |
| brotli L9 | 7,468,536 | 56.85% | 13.5 | 303.3 |
| brotli L6 | 7,489,339 | 57.01% | 33.0 | 304.7 |
| zstd L15 | 7,530,938 | 57.33% | 11.9 | 1003.4 |
| zstd L12 | 7,552,509 | 57.49% | 35.0 | 1001.9 |
| zstd L9 | 7,578,176 | 57.69% | 67.5 | 992.0 |
| brotli L4 | 7,591,522 | 57.79% | 81.7 | 328.4 |
| zstd L5 | 7,619,521 | 58.00% | 126.5 | 976.4 |
| zstd L3 | 7,695,386 | 58.58% | 190.1 | 1063.1 |
| zlib L9 | 7,786,253 | 59.27% | 6.9 | 300.5 |
| zlib L6 | 7,814,687 | 59.49% | 19.9 | 290.6 |
| brotli L1 | 7,855,241 | 59.80% | 265.9 | 288.1 |
| zstd L1 | 7,869,981 | 59.91% | 431.1 | 1351.9 |
| lizard L49 | 8,041,060 | 61.21% | 5.3 | 1517.8 |
| zlib L1 | 8,094,068 | 61.62% | 53.2 | 295.1 |
| lizard L45 | 8,253,375 | 62.83% | 15.1 | 1476.6 |
| lizard L40 | 8,481,600 | 64.57% | 315.2 | 1427.3 |
| lz5 HC L15 | 8,603,507 | 65.49% | 2.1 | 969.2 |
| lz5 HC L12 | 8,720,302 | 66.38% | 10.6 | 1049.8 |
| lizard L30 | 8,756,123 | 66.66% | 440.7 | 1864.4 |
| misa77 L3 | 8,867,044 | 67.50% | 10.2 | 5600.7 |
| lz5 HC L9 | 8,900,815 | 67.76% | 18.9 | 1157.3 |
| **lz6 HC -15** | 8,996,304 | 68.48% | 9.2 | 1609.8 |
| **lz6 HC -12** | 8,996,539 | 68.49% | 9.6 | 1583.7 |
| lz5 HC L6 | 9,066,986 | 69.02% | 42.1 | 1481.3 |
| misa77 L4 | 9,070,575 | 69.05% | 7.4 | 3646.4 |
| **lz6 HC -6** | 9,190,747 | 69.96% | 41.8 | 1531.8 |
| misa77 L2 | 9,287,323 | 70.70% | 38.0 | 6691.5 |
| lz5 | 9,469,498 | 72.09% | 328.1 | 1264.2 |
| misa77 L0 | 9,554,603 | 72.73% | 198.1 | 7421.4 |
| misa77 L1 | 9,628,088 | 73.29% | 51.1 | 7957.0 |
| lizard L20 | 9,761,531 | 74.31% | 441.7 | 2494.8 |
| lizard L10 | 9,926,082 | 75.56% | 603.4 | 4750.7 |
| lz4 | 9,938,605 | 75.66% | 774.0 | 5148.0 |
| misa77 L-1 | 10,041,718 | 76.44% | 313.1 | 7452.0 |
| lz5 HC L1 | 10,441,177 | 79.48% | 535.8 | 2531.0 |
| memlz | 10,724,346 | 81.64% | 1872.5 | 1712.7 |

## Silesia (212 MB, per-file)

### Silesia (212 MB, per-file)

| codec | csize | ratio | enc MB/s | dec MB/s |
|---|---:|---:|---:|---:|
| uf-lzma2 L11 | 48,483,163 | 22.88% | 1.2 | 118.3 |
| fastlzma2 L10 | 48,673,262 | 22.97% | 3.3 | 81.0 |
| uf-lzma2 L10 | 48,673,262 | 22.97% | 3.3 | 110.5 |
| xz L9 | 48,795,480 | 23.02% | 2.4 | 111.8 |
| brotli L11 | 50,328,370 | 23.75% | 0.5 | 361.7 |
| fastlzma2 L5 | 51,124,925 | 24.12% | 5.9 | 77.9 |
| uf-lzma2 L5 | 51,124,925 | 24.12% | 6.1 | 107.0 |
| zstd L19 | 52,891,946 | 24.96% | 2.6 | 755.7 |
| **lz6 seq -15** | 53,416,718 | 25.20% | 0.7 | 667.8 |
| **lz6 seq -13** | 53,479,947 | 25.23% | 1.4 | 676.0 |
| **lz6 seq -12** | 53,636,281 | 25.31% | 2.6 | 665.9 |
| **lz6 seq -10** | 54,594,576 | 25.76% | 4.0 | 651.0 |
| **lz6 seq -8** | 57,390,652 | 27.08% | 7.6 | 591.6 |
| **lz6 seq -7** | 58,046,491 | 27.39% | 15.1 | 627.0 |
| **lz6 seq -5** | 58,534,476 | 27.62% | 28.8 | 652.9 |
| xz L1 | 58,716,448 | 27.70% | 17.1 | 95.6 |
| fastlzma2 L1 | 58,993,508 | 27.84% | 17.8 | 65.1 |
| uf-lzma2 L1 | 58,993,508 | 27.84% | 18.1 | 96.4 |
| **lz6 seq -4** | 59,879,274 | 28.25% | 41.8 | 628.8 |
| lizard L49 | 60,667,327 | 28.62% | 1.8 | 1158.9 |
| **lz6 seq -3** | 61,401,238 | 28.97% | 57.1 | 593.6 |
| zstd L5 | 62,751,501 | 29.61% | 100.6 | 787.3 |
| lz5 HC L15 | 65,595,195 | 30.95% | 2.0 | 769.3 |
| **lz6 seq -2** | 67,069,091 | 31.65% | 135.2 | 536.3 |
| zlib L6 | 68,220,487 | 32.19% | 26.1 | 327.8 |
| **lz6 seq -1** | 70,270,844 | 33.16% | 163.6 | 620.1 |
| zstd L1 | 73,229,468 | 34.55% | 348.0 | 1166.0 |
| brotli L1 | 73,429,961 | 34.65% | 206.4 | 321.5 |
| zlib L1 | 77,246,811 | 36.45% | 84.3 | 305.6 |
| misa77 L3 | 80,028,169 | 37.76% | 10.7 | 4406.4 |
| lz5 HC L6 | 80,576,517 | 38.02% | 45.0 | 1132.8 |
| **lz6 HC -15** | 81,309,280 | 38.36% | 5.4 | 1260.5 |
| lizard L30 | 85,765,250 | 40.47% | 318.4 | 1197.2 |
| lz5 | 88,218,423 | 41.62% | 241.7 | 697.3 |
| misa77 L1 | 90,385,225 | 42.65% | 51.2 | 4932.9 |
| misa77 L-1 | 99,073,179 | 46.75% | 270.9 | 4604.3 |
| lz4 | 100,880,147 | 47.60% | 539.7 | 3537.2 |
| lz5 HC L1 | 113,525,877 | 53.57% | 508.3 | 1615.5 |
| memlz | 126,970,186 | 59.91% | 963.2 | 887.8 |

### Silesia per-file ratio

| file | orig | lz6 L15 | lz6 L2 | zstd -19 | xz -9 | brotli -11 | lizard -49 | lz5 HC15 | lz4 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| dickens | 9.7 MB | 28.29% | 37.33% | 27.96% | 27.77% | 27.99% | 33.00% | 36.87% | 63.07% |
| mozilla | 48.8 MB | 30.20% | 37.26% | 29.41% | 26.11% | 27.48% | 34.31% | 35.79% | 51.61% |
| mr | 9.5 MB | 32.90% | 34.24% | 31.16% | 27.58% | 28.34% | 34.20% | 37.92% | 54.57% |
| nci | 32.0 MB | 5.53% | 9.48% | 4.96% | 5.18% | 4.82% | 5.92% | 7.26% | 16.49% |
| ooffice | 5.9 MB | 38.18% | 46.52% | 42.18% | 39.45% | 40.31% | 50.21% | 49.35% | 70.53% |
| osdb | 9.6 MB | 30.78% | 34.31% | 30.74% | 28.26% | 28.01% | 33.78% | 37.30% | 52.12% |
| reymont | 6.3 MB | 20.88% | 31.85% | 20.35% | 19.87% | 20.15% | 23.34% | 27.09% | 48.00% |
| samba | 20.6 MB | 19.50% | 25.15% | 18.04% | 17.42% | 17.69% | 20.90% | 22.43% | 35.72% |
| sao | 6.9 MB | 69.90% | 75.50% | 68.95% | 60.88% | 63.29% | 73.89% | 78.09% | 93.63% |
| webster | 39.5 MB | 20.67% | 30.43% | 20.93% | 20.23% | 21.20% | 24.64% | 27.00% | 48.58% |
| x-ray | 8.1 MB | 55.85% | 57.49% | 60.53% | 52.98% | 55.30% | 66.91% | 75.46% | 99.01% |
| xml | 5.1 MB | 9.48% | 14.14% | 8.47% | 8.48% | 8.05% | 9.98% | 11.23% | 22.96% |

## Findings

**AIT: the lead is file D.** L15 is 38.70% on the 8 files against brotli
-11's 53.60% and zstd -19's 55.97%, decoding at 572 MB/s. Almost all of that
comes from one file: D is glibc `random()` output, which lz6 regenerates
from its seed (2,000,000 -> 7 bytes) while every other codec stores it raw.
Without D, lz6 -15 is 45.66%: ahead of xz -9 and zstd -19, behind brotli
-11 and LZMA2 (uf-lzma2 -10/-11):

| codec | 8 files | without D | A | B | C | E | F | G | H |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **lz6 -15** | **38.70%** | 45.66% | 55.27% | 18.45% | 27.04% | 79.45% | 79.44% | 29.22% | 38.72% |
| brotli -11 | 53.60% | 45.27% | 50.85% | 17.41% | 24.92% | 84.75% | 79.28% | 30.97% | 36.26% |
| uf-lzma2 -10 | 53.80% | 45.50% | 51.87% | 17.78% | 24.78% | 85.52% | 82.53% | 28.53% | 35.97% |
| xz -9 | 54.16% | 45.92% | 53.34% | 17.76% | 24.72% | 85.84% | 82.38% | 29.71% | 35.87% |
| zstd -19 | 55.97% | 48.06% | 51.77% | 18.06% | 26.66% | 88.52% | 87.83% | 31.61% | 38.49% |
| uf-lzma2 -11 | 52.64% | 44.13% | 51.87% | 17.78% | 24.59% | 78.01% | 79.12% | 28.51% | 35.86% |

Per file, lz6 -15 has the best E among the codecs without a per-file
parameter search (79.53%, uf-lzma2 -11: 78.01%); on F it is within 0.3
points of brotli -11 and uf-lzma2 -11; on A, B and C it still trails
zstd -19 (by 3.5, 0.4 and 0.4 points; on H lz6 is now 0.2 points ahead: the x86 filter), a gap the entropy-priced
parser narrowed from 6.0, 1.2, 2.2 and 2.5. (Until 2026-09-22 these
tables showed lz6 at 46.66% against the other codecs' 8-file totals: the
parser rebuilt each file's size from lzbench's rounded ratio, and D's 0.00
ratio dropped it from lz6's denominator only. Fixed in parse_lzbench.py,
which now uses the real file sizes; reported by uf-lzma2.)

**On Silesia lz6 -15 is 0.24 points behind zstd -19.** L15 is 25.20% @ 668
MB/s versus zstd -19's 24.96% @ 756 MB/s. The gap was 1.56 points before the
entropy-priced parser (v1.6.5-pre), 0.76 before the offset low-bits model
(v1.6.6-pre) and 0.50 before the x86 filter (v1.6.9-pre); zstd -19 decodes
1.13x faster (1.30x in v1.6.7-pre, 2.0x before v1.6.4-pre). lizard -49 is
3.4 points behind lz6 at 1.7x the decode.

**LZMA2 with an assembler decoder.** uf-lzma2 -11 is the best ratio in the
Silesia table (22.88%; -10: 22.97%, xz -9: 23.02%) at ~110-118 MB/s decode;
its -10 streams decode 36-48% faster than the same streams through
fastlzma2's C decoder. That is the ceiling of the bit-wise range-coder
class; lz6 -15 trades 2.3 points of ratio for ~5.5x the decode. Level 11's
per-file lc/lp/pb search also takes AIT/E from 85.52% to 78.01% (lz6's
byte-plane result there: 79.53%).

**memlz** is the opposite end: ~0.9 GB/s both ways on Silesia (1.7-1.9 GB/s
on AIT) at 59.91%, a worse ratio than lz4 (47.60%): a compression-speed codec.

**Per file, lz6 L15 wins x-ray, webster and ooffice** (55.85% vs 60.53%,
20.67% vs 20.93%, 38.18% vs 42.18%), ties osdb (30.78 vs 30.74) and is
within 0.4 points of zstd -19 on dickens (28.29 vs 27.96). The remaining
losses are the structured files: sao 69.90 vs 68.95, mozilla 30.20 vs 29.41,
mr 32.90 vs 31.16 and samba 19.50 vs 18.04.

**x86 branch filter (v1.6.9-pre).** ooffice (a Windows DLL) is the only
Silesia file with x86 code, and the filter takes it from 44.41% to 38.18% at
L15, ahead of xz -9 (39.45%) and brotli -11 (40.31%). CALL/JMP displacements
become absolute addresses, which repeat; it is applied per detected code range
and only where a level-1 trial says it pays, so mozilla (Alpha code) and the
rest are byte-identical. Cost: ~15% of decode time on the code ranges.
Byte-plane lanes are also tried at level 4 and keep Huffman-only literals
(x-ray 57.49% -> 55.85%), and levels 5-6 moved to 32-slot rows (L6 27.69%
at 1.8x the encode speed of the chain parser it replaces).

**Fast end and decode (v1.6.8-pre).** Levels 1-2 use a single-pass
zstd-"fast"-style parser writing sequences straight into the block
collector: lz6 -1 173.7 MB/s encode at 33.33% (v1.6.7-pre: 107 at 33.43%),
lz6 -2 143.9 MB/s at 31.83% (96.6 at 31.86%). zstd -1 is still 2x faster to
encode, at 34.55%. Huffman literals now come in 4 interleaved streams, and
Huffman is no longer rejected on big text (its code lengths are limited
instead): Silesia decode rises across the board (L15 601 -> 677 MB/s, L2
496 -> 543), and AIT from ~435 to 575-684 MB/s.

**Row-hash match finder (v1.6.7-pre).** Levels 3-5 use a zstd 1.5-style
row hash (16 tagged slots per row, one SSE2 compare, lazy parser). lz6 -3 is
now smaller than zstd -5 (29.14% vs 29.61%) at 61 MB/s encode (zstd -5:
101) and 539 MB/s decode (809); lz6 -4 encodes 2x faster than v1.6.6-pre's
-4 at a smaller size. Decode of L4-L7 also stopped choosing 256-context
order-1 literals for 1% gains (2.6x slower to decode on mozilla).

**Offset low bits (v1.6.6-pre).** Offsets used to be coded as a log2 bucket
(entropy-coded) plus raw residual bits, which throws away the alignment of
structured data: record strides and 4/8-byte fields make the low offset bits
highly predictable. The offset symbol now carries the bucket plus the low 3
bits (offsets 1..31 get their own codes), and the optimal parser prices each
(bucket, low bits) cell. Measured offline on dumped parses before touching the
format: bucket slots (deflate/LZMA-style top bits) gained only 0.1%, the low
bits 2.7% on mozilla and sao. Silesia L15 25.72% -> 25.43%, L2 32.07% ->
31.85%; mozilla -3.7%, text ~0; decode +1-3% cycles (a 194-symbol offset
table).

**Encode speed (v1.6.6-pre).** Levels 1-3 encode ~2.5x faster than in
v1.6.5-pre (Silesia -2: 39.3 -> 95.8 MB/s here; -1 106 MB/s), levels 4-7
1.1-1.4x. The biggest single cause was the byte-plane trial, which ran a full
second encode on every text input and never won there; it now also requires
the lanes' entropies to differ. The fast levels also got an 8x smaller hash
(+0.03-0.09 points) and output-identical encoder work (Huffman and bitstream
writers, prefetch).

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

**Decode speed remains lz6's weak axis vs zstd/lizard.** lz6's 540-670
MB/s band sits 1.1-2.2x below zstd (760-1350) and lizard (1160-1930), and
~9-18x below misa77 (4600-7800, at much worse ratio). The seq v2 decoder is
instruction-bound (IPC ~2.5); see the wiki's Decoder Internals and Roadmap
pages.

**Level ladder (v1.6.5-pre).** The seq levels have their own table and form
a strictly monotonic ladder: Silesia L1 33.16% -> L15 25.20%, every level
smaller than the one before (the old table had byte-identical pairs).
L13-L15 differ only by refinement re-parses, so they sit close together
(25.23 / 25.20 / 25.20%); deeper searches gave nothing past 64.

**Not benchmarked here:** brotli/xz are different families with no
fast-decode pretension. The uncomfortable line is still zstd -19, smaller
and 1.13x faster to decode on Silesia; the remaining ratio gap is in the
binary/structured files above.
