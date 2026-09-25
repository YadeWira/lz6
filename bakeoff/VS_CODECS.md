# lz6 vs the field - one harness head-to-head

All numbers below come from a single harness: **lzbench 2.3.1**, one thread
(`-t1,1`), memory-to-memory per file, on an Intel Xeon E5-2697A v4 @ 2.60GHz.
lz6 and lz5 are integrated as lzbench plugins (see
[lzbench_lz6.patch](lzbench_lz6.patch)) so they are timed by the same loop,
with the same buffers and the same round-trip verification as lz4, lizard,
zstd, misa77, brotli, xz and zlib.

Last full sweep: 2026-09-25, v1.6.7-pre (row-hash match finder at levels 3-5,
decode-stage rich tANS entries). fastlzma2 / uf-lzma2 / memlz rows are
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
1.4.x were not re-measured: per its author, nothing changed since 1.2.0 on the
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
| **lz6 seq -15** | 5,100,395 | 38.83% | 1.6 | 436.2 |
| **lz6 seq -14** | 5,101,443 | 38.83% | 1.9 | 425.0 |
| **lz6 seq -13** | 5,104,969 | 38.86% | 3.2 | 435.4 |
| **lz6 seq -12** | 5,114,516 | 38.93% | 6.2 | 436.3 |
| **lz6 seq -11** | 5,117,662 | 38.96% | 8.8 | 431.5 |
| **lz6 seq -10** | 5,119,428 | 38.97% | 9.0 | 431.4 |
| **lz6 seq -9** | 5,134,824 | 39.09% | 9.9 | 429.9 |
| **lz6 seq -8** | 5,155,127 | 39.24% | 12.0 | 428.5 |
| **lz6 seq -7** | 5,192,768 | 39.53% | 22.6 | 433.3 |
| **lz6 seq -6** | 5,197,867 | 39.57% | 25.6 | 433.4 |
| **lz6 seq -5** | 5,201,042 | 39.59% | 61.6 | 448.0 |
| **lz6 seq -4** | 5,210,499 | 39.66% | 65.4 | 448.3 |
| **lz6 seq -3** | 5,241,400 | 39.90% | 96.9 | 438.3 |
| **lz6 seq -2** | 5,439,718 | 41.41% | 139.9 | 440.6 |
| **lz6 seq -1** | 5,569,986 | 42.40% | 163.6 | 449.2 |
| uf-lzma2 L11 | 6,914,570 | 52.64% | 2.2 | 64.8 |
| brotli L11 | 7,041,245 | 53.60% | 0.6 | 223.8 |
| fastlzma2 L10 | 7,067,576 | 53.80% | 6.2 | 47.0 |
| uf-lzma2 L10 | 7,067,576 | 53.80% | 6.2 | 63.9 |
| fastlzma2 L5 | 7,086,428 | 53.95% | 8.6 | 46.5 |
| uf-lzma2 L5 | 7,086,428 | 53.95% | 8.6 | 62.8 |
| xz L6 | 7,114,024 | 54.16% | 3.9 | 68.6 |
| xz L9 | 7,114,024 | 54.16% | 3.3 | 67.6 |
| fastlzma2 L1 | 7,330,324 | 55.80% | 16.3 | 49.3 |
| uf-lzma2 L1 | 7,330,324 | 55.80% | 16.4 | 70.4 |
| zstd L19 | 7,351,946 | 55.97% | 5.2 | 1070.2 |
| xz L3 | 7,379,360 | 56.18% | 6.7 | 65.7 |
| xz L1 | 7,435,760 | 56.60% | 9.0 | 64.6 |
| brotli L9 | 7,468,536 | 56.85% | 13.6 | 313.1 |
| brotli L6 | 7,489,339 | 57.01% | 33.0 | 315.4 |
| zstd L15 | 7,530,938 | 57.33% | 13.3 | 1002.7 |
| zstd L12 | 7,552,509 | 57.49% | 36.3 | 1002.1 |
| zstd L9 | 7,578,176 | 57.69% | 68.7 | 1013.3 |
| brotli L4 | 7,591,522 | 57.79% | 80.3 | 338.5 |
| zstd L5 | 7,619,521 | 58.00% | 127.3 | 977.7 |
| zstd L3 | 7,695,386 | 58.58% | 190.0 | 1063.5 |
| zlib L9 | 7,786,253 | 59.27% | 7.2 | 313.2 |
| zlib L6 | 7,814,687 | 59.49% | 20.6 | 303.9 |
| brotli L1 | 7,855,241 | 59.80% | 260.2 | 295.8 |
| zstd L1 | 7,869,981 | 59.91% | 435.9 | 1378.4 |
| lizard L49 | 8,041,060 | 61.21% | 5.5 | 1554.9 |
| zlib L1 | 8,094,068 | 61.62% | 54.5 | 304.7 |
| lizard L45 | 8,253,375 | 62.83% | 16.9 | 1556.3 |
| lizard L40 | 8,481,600 | 64.57% | 332.6 | 1504.6 |
| lz5 HC L15 | 8,603,507 | 65.49% | 2.1 | 974.0 |
| lz5 HC L12 | 8,720,302 | 66.38% | 10.1 | 1060.6 |
| lizard L30 | 8,756,123 | 66.66% | 451.5 | 1927.1 |
| misa77 L3 | 8,867,044 | 67.50% | 9.6 | 5465.5 |
| lz5 HC L9 | 8,900,815 | 67.76% | 20.4 | 1157.3 |
| **lz6 HC -15** | 8,996,054 | 68.48% | 10.2 | 1660.5 |
| **lz6 HC -12** | 8,996,268 | 68.48% | 11.2 | 1653.4 |
| lz5 HC L6 | 9,066,986 | 69.02% | 41.2 | 1526.9 |
| misa77 L4 | 9,070,575 | 69.05% | 7.4 | 3453.2 |
| **lz6 HC -6** | 9,190,747 | 69.96% | 42.9 | 1600.2 |
| misa77 L2 | 9,287,323 | 70.70% | 38.3 | 6600.9 |
| lz5 | 9,469,498 | 72.09% | 317.3 | 1247.9 |
| misa77 L0 | 9,554,603 | 72.73% | 209.3 | 7300.5 |
| misa77 L1 | 9,628,088 | 73.29% | 50.3 | 7798.3 |
| lizard L20 | 9,761,531 | 74.31% | 450.3 | 2506.1 |
| lizard L10 | 9,926,082 | 75.56% | 628.4 | 4725.5 |
| lz4 | 9,938,605 | 75.66% | 782.1 | 5086.0 |
| misa77 L-1 | 10,041,718 | 76.44% | 327.0 | 7448.0 |
| lz5 HC L1 | 10,441,177 | 79.48% | 537.1 | 2517.6 |
| memlz | 10,724,346 | 81.64% | 1872.5 | 1712.7 |

## Silesia (212 MB, per-file)

### Silesia (212 MB, per-file)

| codec | csize | ratio | enc MB/s | dec MB/s |
|---|---:|---:|---:|---:|
| uf-lzma2 L11 | 48,483,163 | 22.88% | 1.2 | 118.3 |
| fastlzma2 L10 | 48,673,262 | 22.97% | 3.3 | 81.0 |
| uf-lzma2 L10 | 48,673,262 | 22.97% | 3.3 | 110.5 |
| xz L9 | 48,795,480 | 23.02% | 2.5 | 112.7 |
| brotli L11 | 50,328,370 | 23.75% | 0.5 | 362.0 |
| fastlzma2 L5 | 51,124,925 | 24.12% | 5.9 | 77.9 |
| uf-lzma2 L5 | 51,124,925 | 24.12% | 6.1 | 107.0 |
| zstd L19 | 52,891,946 | 24.96% | 2.8 | 784.4 |
| **lz6 seq -15** | 53,929,126 | 25.45% | 0.7 | 600.9 |
| **lz6 seq -13** | 53,992,880 | 25.48% | 1.4 | 615.1 |
| **lz6 seq -12** | 54,151,986 | 25.55% | 2.8 | 616.8 |
| **lz6 seq -10** | 55,107,589 | 26.00% | 4.3 | 610.0 |
| **lz6 seq -8** | 57,874,091 | 27.31% | 8.1 | 562.3 |
| **lz6 seq -7** | 58,543,789 | 27.62% | 16.2 | 542.2 |
| xz L1 | 58,716,448 | 27.70% | 16.0 | 96.9 |
| fastlzma2 L1 | 58,993,508 | 27.84% | 17.8 | 65.1 |
| uf-lzma2 L1 | 58,993,508 | 27.84% | 18.1 | 96.4 |
| **lz6 seq -5** | 59,460,886 | 28.06% | 36.2 | 576.7 |
| **lz6 seq -4** | 60,412,245 | 28.50% | 45.8 | 560.2 |
| lizard L49 | 60,667,327 | 28.62% | 1.9 | 1189.8 |
| **lz6 seq -3** | 61,766,808 | 29.14% | 61.1 | 538.8 |
| zstd L5 | 62,751,501 | 29.61% | 100.6 | 809.2 |
| lz5 HC L15 | 65,595,195 | 30.95% | 2.1 | 764.7 |
| **lz6 seq -2** | 67,528,503 | 31.86% | 96.6 | 496.0 |
| zlib L6 | 68,220,487 | 32.19% | 26.9 | 332.5 |
| **lz6 seq -1** | 70,858,255 | 33.43% | 107.1 | 485.2 |
| zstd L1 | 73,229,468 | 34.55% | 351.5 | 1203.4 |
| brotli L1 | 73,429,961 | 34.65% | 209.4 | 331.4 |
| zlib L1 | 77,246,811 | 36.45% | 84.6 | 306.7 |
| misa77 L3 | 80,028,169 | 37.76% | 11.0 | 4605.2 |
| lz5 HC L6 | 80,576,517 | 38.02% | 48.5 | 1177.3 |
| **lz6 HC -15** | 81,308,486 | 38.36% | 5.5 | 1283.6 |
| lizard L30 | 85,765,250 | 40.47% | 322.5 | 1219.6 |
| lz5 | 88,218,423 | 41.62% | 234.7 | 712.3 |
| misa77 L1 | 90,385,225 | 42.65% | 51.3 | 5157.1 |
| misa77 L-1 | 99,073,179 | 46.75% | 280.2 | 4800.6 |
| lz4 | 100,880,147 | 47.60% | 546.2 | 3639.4 |
| lz5 HC L1 | 113,525,877 | 53.57% | 512.0 | 1659.5 |
| memlz | 126,970,186 | 59.91% | 963.2 | 887.8 |

### Silesia per-file ratio

| file | orig | lz6 L15 | lz6 L2 | zstd -19 | xz -9 | brotli -11 | lizard -49 | lz5 HC15 | lz4 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| dickens | 9.7 MB | 28.29% | 37.60% | 27.96% | 27.77% | 27.99% | 33.00% | 36.87% | 63.07% |
| mozilla | 48.8 MB | 30.20% | 37.55% | 29.41% | 26.11% | 27.48% | 34.31% | 35.79% | 51.61% |
| mr | 9.5 MB | 33.24% | 33.86% | 31.16% | 27.58% | 28.34% | 34.20% | 37.92% | 54.57% |
| nci | 32.0 MB | 5.52% | 8.67% | 4.96% | 5.18% | 4.82% | 5.92% | 7.26% | 16.49% |
| ooffice | 5.9 MB | 44.41% | 57.44% | 42.18% | 39.45% | 40.31% | 50.21% | 49.35% | 70.53% |
| osdb | 9.6 MB | 30.78% | 34.56% | 30.74% | 28.26% | 28.01% | 33.78% | 37.30% | 52.12% |
| reymont | 6.3 MB | 20.88% | 31.42% | 20.35% | 19.87% | 20.15% | 23.34% | 27.09% | 48.00% |
| samba | 20.6 MB | 19.50% | 25.42% | 18.04% | 17.42% | 17.69% | 20.90% | 22.43% | 35.72% |
| sao | 6.9 MB | 69.90% | 79.47% | 68.95% | 60.88% | 63.29% | 73.89% | 78.09% | 93.63% |
| webster | 39.5 MB | 20.67% | 29.62% | 20.93% | 20.23% | 21.20% | 24.64% | 27.00% | 48.58% |
| x-ray | 8.1 MB | 57.02% | 57.02% | 60.53% | 52.98% | 55.30% | 66.91% | 75.46% | 99.01% |
| xml | 5.1 MB | 9.48% | 13.32% | 8.47% | 8.48% | 8.05% | 9.98% | 11.23% | 22.96% |

## Findings

**AIT: the lead is file D.** L15 is 38.83% on the 8 files against brotli
-11's 53.60% and zstd -19's 55.97%, decoding at 436 MB/s. Almost all of that
comes from one file: D is glibc `random()` output, which lz6 regenerates
from its seed (2,000,000 -> 7 bytes) while every other codec stores it raw.
Without D, lz6 -15 is 45.80%: ahead of xz -9 and zstd -19, behind brotli
-11 and LZMA2 (uf-lzma2 -10/-11):

| codec | 8 files | without D | A | B | C | E | F | G | H |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **lz6 -15** | **38.83%** | 45.80% | 55.27% | 18.44% | 27.05% | 79.45% | 79.39% | 29.32% | 40.14% |
| brotli -11 | 53.60% | 45.27% | 50.85% | 17.41% | 24.92% | 84.75% | 79.28% | 30.97% | 36.26% |
| uf-lzma2 -10 | 53.80% | 45.50% | 51.87% | 17.78% | 24.78% | 85.52% | 82.53% | 28.53% | 35.97% |
| xz -9 | 54.16% | 45.92% | 53.34% | 17.76% | 24.72% | 85.84% | 82.38% | 29.71% | 35.87% |
| zstd -19 | 55.97% | 48.06% | 51.77% | 18.06% | 26.66% | 88.52% | 87.83% | 31.61% | 38.49% |
| uf-lzma2 -11 | 52.64% | 44.13% | 51.87% | 17.78% | 24.59% | 78.01% | 79.12% | 28.51% | 35.86% |

Per file, lz6 -15 has the best E among the codecs without a per-file
parameter search (79.53%, uf-lzma2 -11: 78.01%); on F it is within 0.3
points of brotli -11 and uf-lzma2 -11; on A, B, C and H it still trails
zstd -19 (by 3.5, 0.4, 0.4 and 1.7 points), a gap the entropy-priced
parser narrowed from 6.0, 1.2, 2.2 and 2.5. (Until 2026-09-22 these
tables showed lz6 at 46.66% against the other codecs' 8-file totals: the
parser rebuilt each file's size from lzbench's rounded ratio, and D's 0.00
ratio dropped it from lz6's denominator only. Fixed in parse_lzbench.py,
which now uses the real file sizes; reported by uf-lzma2.)

**On Silesia lz6 -15 is 0.49 points behind zstd -19.** L15 is 25.45% @ 601
MB/s versus zstd -19's 24.96% @ 784 MB/s. The gap was 1.56 points before the
entropy-priced parser (v1.6.5-pre) and 0.76 before the offset low-bits model
(v1.6.6-pre); zstd -19 decodes 1.30x faster (2.0x before v1.6.4-pre).
lizard -49 is 3.2 points behind lz6 at 2.0x the decode.

**LZMA2 with an assembler decoder.** uf-lzma2 -11 is the best ratio in the
Silesia table (22.88%; -10: 22.97%, xz -9: 23.02%) at ~110-118 MB/s decode;
its -10 streams decode 36-48% faster than the same streams through
fastlzma2's C decoder. That is the ceiling of the bit-wise range-coder
class; lz6 -15 trades 2.6 points of ratio for ~5x the decode. Level 11's
per-file lc/lp/pb search also takes AIT/E from 85.52% to 78.01% (lz6's
byte-plane result there: 79.53%).

**memlz** is the opposite end: ~0.9 GB/s both ways on Silesia (1.7-1.9 GB/s
on AIT) at 59.91%, a worse ratio than lz4 (47.60%): a compression-speed codec.

**Per file, lz6 L15 wins x-ray and webster** (57.02% vs 60.53%, 20.67%
vs 20.93%), ties osdb (30.78 vs 30.74) and is within 0.4 points of zstd -19
on dickens (28.29 vs 27.96). The remaining losses are the binary/structured
files: sao 69.90 vs 68.95, mozilla 30.20 vs 29.41, ooffice 44.41 vs 42.18,
mr 33.24 vs 31.16 and samba 19.50 vs 18.04.

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

**Decode speed remains lz6's weak axis vs zstd/lizard.** lz6's 440-620
MB/s band sits 1.3-3x below zstd (780-1380) and lizard (1200-1930), and
~9-18x below misa77 (4600-7800, at much worse ratio). The seq v2 decoder is
instruction-bound (IPC ~2.5); see the wiki's Decoder Internals and Roadmap
pages.

**Level ladder (v1.6.5-pre).** The seq levels have their own table and form
a strictly monotonic ladder: Silesia L1 33.43% -> L15 25.45%, every level
smaller than the one before (the old table had byte-identical pairs).
L13-L15 differ only by refinement re-parses, so they sit close together
(25.48 / 25.45 / 25.45%); deeper searches gave nothing past 64.

**Not benchmarked here:** brotli/xz are different families with no
fast-decode pretension. The uncomfortable line is still zstd -19, smaller
and 1.30x faster to decode on Silesia; the remaining ratio gap is in the
binary/structured files above.
