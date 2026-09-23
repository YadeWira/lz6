# lz6 vs the field - one harness head-to-head

All numbers below come from a single harness: **lzbench 2.3.1**, one thread
(`-t1,1`), memory-to-memory per file, on an Intel Xeon E5-2697A v4 @ 2.60GHz.
lz6 and lz5 are integrated as lzbench plugins (see
[lzbench_lz6.patch](lzbench_lz6.patch)) so they are timed by the same loop,
with the same buffers and the same round-trip verification as lz4, lizard,
zstd, misa77, brotli, xz and zlib.

Last full sweep: 2026-09-22, master `8c1713c` (the v1.6.4-pre seq decoder
and the 12-bit rANS precision cap). Added the same night on the same
machine and settings: **fastlzma2** 1.0.1 (lzbench's build, C decoder),
**uf-lzma2** 1.1.0 (github.com/YadeWira/ultra-fast-lzma2, a fast-lzma2 fork,
with Igor Pavlov's assembler LZMA decoder: same LZMA2 stream, compressed
sizes byte-identical to fastlzma2 on every file) and **memlz** 0.2 beta.
uf-lzma2 enters through its own lzbench patch, reviewed before building.

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
   (L15) vs 26.39% here, i.e. per-file separation is still the ideal and
   segmentation only recovers part of it.
2. Encode speeds for the heavy codecs are single-iteration-ish at these
   sizes; treat them as order-of-magnitude, not precise.

## AIT A-H (16 MB)

### AIT A-H (16 MB)

| codec | csize | ratio | enc MB/s | dec MB/s |
|---|---:|---:|---:|---:|
| **lz6 seq -13** | 5,195,790 | 46.66% | 5.2 | 286.4 |
| **lz6 seq -14** | 5,195,809 | 46.66% | 4.2 | 294.9 |
| **lz6 seq -15** | 5,195,697 | 46.66% | 1.7 | 291.4 |
| **lz6 seq -11** | 5,199,040 | 46.68% | 7.8 | 302.1 |
| **lz6 seq -12** | 5,198,938 | 46.68% | 7.7 | 295.5 |
| **lz6 seq -10** | 5,222,695 | 46.90% | 16.3 | 311.4 |
| **lz6 seq -9** | 5,241,048 | 47.06% | 16.9 | 311.2 |
| **lz6 seq -6** | 5,285,184 | 47.46% | 23.6 | 303.9 |
| **lz6 seq -7** | 5,285,184 | 47.46% | 23.7 | 306.4 |
| **lz6 seq -8** | 5,285,197 | 47.46% | 23.9 | 298.0 |
| **lz6 seq -3** | 5,375,855 | 48.27% | 60.4 | 326.6 |
| **lz6 seq -4** | 5,404,438 | 48.53% | 45.6 | 308.3 |
| **lz6 seq -5** | 5,404,438 | 48.53% | 46.2 | 307.8 |
| **lz6 seq -2** | 5,433,041 | 48.79% | 79.3 | 359.4 |
| **lz6 seq -1** | 5,561,279 | 49.94% | 87.4 | 372.5 |
| brotli L11 | 7,041,245 | 53.60% | 0.5 | 210.5 |
| fastlzma2 L10 | 7,067,576 | 53.80% | 6.2 | 47.0 |
| uf-lzma2 L10 (asm dec) | 7,067,576 | 53.80% | 6.2 | 63.9 |
| fastlzma2 L5 | 7,086,428 | 53.95% | 8.6 | 46.5 |
| uf-lzma2 L5 (asm dec) | 7,086,428 | 53.95% | 8.6 | 62.8 |
| xz L6 | 7,114,024 | 54.16% | 3.3 | 65.9 |
| xz L9 | 7,114,024 | 54.16% | 3.0 | 63.6 |
| fastlzma2 L1 | 7,330,324 | 55.80% | 16.3 | 49.3 |
| uf-lzma2 L1 (asm dec) | 7,330,324 | 55.80% | 16.4 | 70.4 |
| zstd L19 | 7,351,946 | 55.97% | 4.7 | 1052.6 |
| xz L3 | 7,379,360 | 56.18% | 6.0 | 62.2 |
| xz L1 | 7,435,760 | 56.60% | 8.3 | 60.6 |
| brotli L9 | 7,468,536 | 56.86% | 13.0 | 303.3 |
| brotli L6 | 7,489,339 | 57.01% | 30.8 | 297.5 |
| zstd L15 | 7,530,938 | 57.33% | 12.9 | 1022.9 |
| zstd L12 | 7,552,509 | 57.49% | 36.1 | 1012.8 |
| zstd L9 | 7,578,176 | 57.69% | 64.3 | 996.3 |
| brotli L4 | 7,591,522 | 57.79% | 73.4 | 326.6 |
| zstd L5 | 7,619,521 | 58.00% | 126.9 | 962.0 |
| zstd L3 | 7,695,386 | 58.58% | 192.4 | 1062.9 |
| zlib L9 | 7,786,253 | 59.27% | 6.8 | 287.0 |
| zlib L6 | 7,814,687 | 59.49% | 19.9 | 294.5 |
| brotli L1 | 7,855,241 | 59.80% | 255.9 | 278.4 |
| zstd L1 | 7,869,981 | 59.91% | 432.4 | 1342.2 |
| lizard L49 | 8,041,060 | 61.21% | 5.1 | 1552.8 |
| zlib L1 | 8,094,068 | 61.62% | 53.1 | 291.9 |
| lizard L45 | 8,253,375 | 62.83% | 14.9 | 1543.6 |
| lizard L40 | 8,481,600 | 64.57% | 329.8 | 1491.8 |
| lz5 HC L15 | 8,603,507 | 65.49% | 2.0 | 919.8 |
| lz5 HC L12 | 8,720,302 | 66.38% | 10.5 | 990.0 |
| lizard L30 | 8,756,123 | 66.66% | 430.4 | 1909.1 |
| misa77 L3 | 8,867,044 | 67.50% | 9.6 | 5471.3 |
| lz5 HC L9 | 8,900,815 | 67.76% | 18.3 | 1076.7 |
| **lz6 HC -15** | 8,996,054 | 68.48% | 10.1 | 1653.0 |
| **lz6 HC -12** | 8,996,268 | 68.49% | 10.9 | 1655.8 |
| lz5 HC L6 | 9,066,986 | 69.02% | 37.9 | 1348.0 |
| misa77 L4 | 9,070,575 | 69.05% | 7.0 | 3534.7 |
| **lz6 HC -6** | 9,190,747 | 69.97% | 41.6 | 1567.1 |
| misa77 L2 | 9,287,323 | 70.70% | 36.0 | 6587.5 |
| lz5 | 9,469,498 | 72.09% | 306.1 | 1162.0 |
| misa77 L0 | 9,554,603 | 72.73% | 213.0 | 7255.0 |
| misa77 L1 | 9,628,088 | 73.29% | 48.9 | 7762.4 |
| lizard L20 | 9,761,531 | 74.31% | 443.3 | 2472.3 |
| lizard L10 | 9,926,082 | 75.56% | 610.2 | 4684.3 |
| lz4 | 9,938,605 | 75.66% | 689.3 | 4637.7 |
| misa77 L-1 | 10,041,718 | 76.44% | 327.8 | 7400.5 |
| lz5 HC L1 | 10,441,177 | 79.48% | 492.1 | 2259.5 |
| memlz | 10,724,346 | 81.64% | 1872.5 | 1712.6 |

## Silesia (212 MB, per-file)

### Silesia (212 MB, per-file)

| codec | csize | ratio | enc MB/s | dec MB/s |
|---|---:|---:|---:|---:|
| fastlzma2 L10 | 48,673,262 | 22.96% | 3.3 | 81.0 |
| uf-lzma2 L10 (asm dec) | 48,673,262 | 22.96% | 3.3 | 110.5 |
| xz L9 | 48,795,480 | 23.02% | 2.5 | 110.3 |
| brotli L11 | 50,328,370 | 23.75% | 0.5 | 358.5 |
| fastlzma2 L5 | 51,124,925 | 24.12% | 5.9 | 77.9 |
| uf-lzma2 L5 (asm dec) | 51,124,925 | 24.12% | 6.1 | 107.0 |
| zstd L19 | 52,891,946 | 24.95% | 2.7 | 789.6 |
| **lz6 seq -15** | 55,929,013 | 26.39% | 2.2 | 399.0 |
| **lz6 seq -11** | 56,423,053 | 26.62% | 3.8 | 393.4 |
| xz L1 | 58,716,448 | 27.71% | 16.3 | 94.2 |
| fastlzma2 L1 | 58,993,508 | 27.84% | 17.8 | 65.1 |
| uf-lzma2 L1 (asm dec) | 58,993,508 | 27.84% | 18.1 | 96.4 |
| lizard L49 | 60,667,327 | 28.62% | 1.9 | 1197.6 |
| **lz6 seq -9** | 61,487,881 | 29.01% | 16.8 | 356.2 |
| zstd L5 | 62,751,501 | 29.60% | 101.6 | 808.3 |
| lz5 HC L15 | 65,595,195 | 30.95% | 2.1 | 750.3 |
| **lz6 seq -5** | 67,364,556 | 31.79% | 32.4 | 319.6 |
| **lz6 seq -3** | 67,590,963 | 31.89% | 35.2 | 341.0 |
| **lz6 seq -2** | 67,645,372 | 31.92% | 38.4 | 363.5 |
| zlib L6 | 68,220,487 | 32.19% | 26.6 | 326.1 |
| **lz6 seq -1** | 70,883,628 | 33.44% | 41.3 | 372.1 |
| zstd L1 | 73,229,468 | 34.55% | 358.0 | 1205.3 |
| brotli L1 | 73,429,961 | 34.65% | 211.8 | 333.4 |
| zlib L1 | 77,246,811 | 36.45% | 83.5 | 303.9 |
| misa77 L3 | 80,028,169 | 37.76% | 11.0 | 4620.8 |
| lz5 HC L6 | 80,576,517 | 38.02% | 47.7 | 1148.0 |
| **lz6 HC -15** | 81,308,555 | 38.37% | 5.6 | 1296.5 |
| lizard L30 | 85,765,250 | 40.47% | 322.5 | 1226.8 |
| lz5 | 88,218,423 | 41.62% | 234.9 | 706.6 |
| misa77 L1 | 90,385,225 | 42.65% | 51.1 | 5153.7 |
| misa77 L-1 | 99,073,179 | 46.75% | 280.9 | 4791.0 |
| lz4 | 100,880,147 | 47.60% | 540.7 | 3579.7 |
| lz5 HC L1 | 113,525,877 | 53.56% | 505.6 | 1640.2 |
| memlz | 126,970,186 | 59.91% | 963.2 | 887.8 |

### Silesia per-file ratio

| file | orig | lz6 L15 | lz6 L2 | zstd -19 | xz -9 | brotli -11 | lizard -49 | lz5 HC15 | lz4 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| dickens | 9.7 MB | 30.19% | 37.45% | 27.96% | 27.77% | 27.99% | 33.00% | 36.87% | 63.07% |
| mozilla | 48.8 MB | 31.75% | 38.04% | 29.41% | 26.11% | 27.48% | 34.31% | 35.79% | 51.61% |
| mr | 9.5 MB | 33.40% | 33.87% | 31.16% | 27.58% | 28.34% | 34.20% | 37.92% | 54.57% |
| nci | 32.0 MB | 5.79% | 8.51% | 4.96% | 5.18% | 4.82% | 5.92% | 7.26% | 16.49% |
| ooffice | 5.9 MB | 45.38% | 56.97% | 42.18% | 39.45% | 40.31% | 50.21% | 49.35% | 70.53% |
| osdb | 9.6 MB | 31.67% | 34.76% | 30.74% | 28.26% | 28.01% | 33.78% | 37.30% | 52.12% |
| reymont | 6.3 MB | 22.30% | 31.32% | 20.35% | 19.87% | 20.15% | 23.34% | 27.09% | 48.00% |
| samba | 20.6 MB | 20.11% | 25.36% | 18.04% | 17.42% | 17.69% | 20.90% | 22.43% | 35.72% |
| sao | 6.9 MB | 71.46% | 79.75% | 68.95% | 60.88% | 63.29% | 73.89% | 78.09% | 93.63% |
| webster | 39.5 MB | 21.70% | 29.53% | 20.93% | 20.23% | 21.20% | 24.64% | 27.00% | 48.58% |
| x-ray | 8.1 MB | 56.73% | 56.73% | 60.53% | 52.98% | 55.30% | 66.91% | 75.46% | 99.01% |
| xml | 5.1 MB | 9.78% | 13.31% | 8.47% | 8.48% | 8.05% | 9.98% | 11.23% | 22.96% |

## Findings

**lz6 owns AIT.** L15 is 46.66% against brotli -11's 53.60% - a 6.9 point gap
- while decoding at 291 MB/s (brotli -11: 211 MB/s, xz -9: 64 MB/s). Against
zstd -19 it is 9.3 points smaller for a 3.6x slower decode (zstd -19:
1053 MB/s).

**On Silesia lz6 -15 is 1.4 points behind zstd -19 and now out-decodes
brotli -11.** L15 is 26.39% @ 399 MB/s versus zstd -19's 24.95% @ 790 MB/s
and brotli -11's 23.75% @ 359 MB/s. zstd -19 still beats lz6 on both axes;
brotli -11 is smaller but decodes slower. lizard -49 is 2.2 points behind at
3.0x the decode (1198 MB/s).

**LZMA2 with an assembler decoder.** uf-lzma2 -10 is the best ratio in the
Silesia table (22.96%, xz -9: 23.02%) at 110 MB/s decode, 36-48% faster than
the same streams through fastlzma2's C decoder. That is the ceiling of the
bit-wise range-coder class; lz6 -15 trades 3.4 points of ratio for 3.6x the
decode.

**memlz** is the opposite end: ~0.9 GB/s both ways on Silesia (1.7-1.9 GB/s
on AIT) at 59.91%, a worse ratio than lz4 (47.60%): a compression-speed codec.

**Per file, lz6 L15 wins only x-ray** (56.73% vs zstd's 60.53%), but it is
within a point on webster (21.70 vs 20.93) and osdb (31.67 vs 30.74), and
within 2.3 on mr and dickens. The remaining big losses are mozilla (31.75 vs
29.41, xz 26.11), sao (71.46 vs 68.95) and ooffice (45.38 vs 42.18). Those
are the binary/structured cases where the literal coder still goes raw.

**The decoder rewrite (v1.6.4-pre).** Same harness, same machine, lz6 seq
decode before -> after: Silesia L15 274 -> 399 MB/s (+46%), L9 239 -> 356,
L2 255 -> 364, L1 260 -> 372; AIT L15 234 -> 291 (+25%), L2 278 -> 359.
Literals are decoded in bulk (two-symbol Huffman lookups), the rANS renorm
is branch-free with L1-resident 4-byte table entries, and a two-stage
sequence loop prefetches match sources 16 sequences ahead. Sizes moved by
+0.01-0.04% from the 12-bit rANS precision cap. Details: the wiki's
Decoder Internals page.

**The repcode merge - why it worked.** Instrumenting the encoder showed
offsets dominate: 42-82% of the output (dickens L15: 2.75 MB of 3.36 MB),
with only 12% of sequences using a repcode - yet every sequence paid a fixed
2-bit rep field. Folding the repcode into the offset FSE alphabet codes the
joint (bucket, repcode) distribution instead, saving ~1.4 bits per sequence
on text (measured: Silesia tar L15 62,437,010 -> 58,693,799, -6.0%; AIT
5,367,989 -> 5,194,253, -3.2%) and removing a bitfield read, so decode got
~6% faster as well. It also breaks the seq block format - see NEWS.

**Decode speed remains lz6's structural weakness vs zstd/lizard.** lz6's
290-400 MB/s band sits 2-4x below zstd (790-1340) and lizard (1200-1900),
and 12-27x below misa77 (4600-7800, at much worse ratio). What remains is
mostly format-level: rANS with a multiply per symbol, single-state streams,
25 per-bucket offset streams and single-stream Huffman (see the wiki's
Roadmap page).

**Level redundancy is still there.** L6 == L7 and L4 == L5 byte-identical on
AIT, and L11-L15 span 0.02 points (46.66-46.68%) while encode cost ranges
from 7.8 down to 1.7 MB/s. L12 could be dropped with no ratio loss; L13/L14
are effectively L15. On AIT the fastest-decoding seq level is L1 (373 MB/s).

**Not benchmarked here:** brotli/xz are different families with no
fast-decode pretension. The uncomfortable line is now zstd -19, which is
both smaller and 2x faster to decode on Silesia; closing the ratio part
means fixing mozilla-class literals.
