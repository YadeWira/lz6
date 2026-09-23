# lz6 vs the field - one harness head-to-head

> **Update (v1.6.4-pre decode work):** the lz6 rows below predate the seq
> decoder rewrite. A focused lzbench rerun on the same machine (lz6
> -1/-2/-9/-15 against zstd, lizard, misa77, lz4, lz5) gives Silesia
> L15 373 MB/s (was 274), L9 345 (239), L2 364 (255); AIT L15 300 (234),
> L2 362 (278). Sizes moved by +0.01-0.04% from the 12-bit rANS precision
> cap. The README table uses those numbers; a full sweep refresh is
> pending.

All numbers below come from a single harness: **lzbench 2.3.1**, one thread
(`-t1,1`), memory-to-memory per file, on an Intel Xeon E5-2697A v4 @ 2.60GHz.
lz6 and lz5 are integrated as lzbench plugins (see
[lzbench_lz6.patch](lzbench_lz6.patch)) so they are timed by the same loop,
with the same buffers and the same round-trip verification as lz4, lizard,
zstd, misa77, brotli, xz and zlib.

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
| **lz6 1.6.3-pre seq -14** | 5,194,214 | 46.64% | 4.7 | 235.4 |
| **lz6 1.6.3-pre seq -13** | 5,194,195 | 46.64% | 5.6 | 234.8 |
| **lz6 1.6.3-pre seq -15** | 5,194,101 | 46.64% | 1.9 | 233.6 |
| **lz6 1.6.3-pre seq -11** | 5,197,437 | 46.67% | 8.1 | 234.6 |
| **lz6 1.6.3-pre seq -12** | 5,197,336 | 46.67% | 7.9 | 234.8 |
| **lz6 1.6.3-pre seq -10** | 5,221,207 | 46.89% | 17.0 | 239.5 |
| **lz6 1.6.3-pre seq -9** | 5,239,520 | 47.05% | 18.1 | 236.8 |
| **lz6 1.6.3-pre seq -6** | 5,283,587 | 47.45% | 24.6 | 225.8 |
| **lz6 1.6.3-pre seq -7** | 5,283,587 | 47.45% | 24.8 | 231.8 |
| **lz6 1.6.3-pre seq -8** | 5,283,615 | 47.45% | 24.6 | 230.7 |
| **lz6 1.6.3-pre seq -3** | 5,374,292 | 48.26% | 61.6 | 251.6 |
| **lz6 1.6.3-pre seq -4** | 5,402,830 | 48.51% | 47.1 | 230.5 |
| **lz6 1.6.3-pre seq -5** | 5,402,830 | 48.51% | 47.2 | 231.2 |
| **lz6 1.6.3-pre seq -2** | 5,431,108 | 48.77% | 77.6 | 277.7 |
| **lz6 1.6.3-pre seq -1** | 5,559,022 | 49.92% | 87.1 | 275.9 |
| brotli L11 | 7,041,245 | 53.60% | 0.6 | 222.6 |
| xz L6 | 7,114,024 | 54.16% | 3.9 | 67.4 |
| xz L9 | 7,114,024 | 54.16% | 3.2 | 66.6 |
| zstd L19 | 7,351,946 | 55.97% | 5.5 | 1103.6 |
| xz L3 | 7,379,360 | 56.18% | 6.9 | 66.6 |
| xz L1 | 7,435,760 | 56.60% | 8.9 | 64.5 |
| brotli L9 | 7,468,536 | 56.86% | 13.9 | 314.5 |
| brotli L6 | 7,489,339 | 57.01% | 33.4 | 315.8 |
| zstd L15 | 7,530,938 | 57.33% | 13.7 | 1035.3 |
| zstd L12 | 7,552,509 | 57.49% | 37.8 | 1028.5 |
| zstd L9 | 7,578,176 | 57.69% | 67.8 | 1031.9 |
| brotli L4 | 7,591,522 | 57.79% | 81.8 | 339.6 |
| zstd L5 | 7,619,521 | 58.00% | 129.5 | 998.2 |
| zstd L3 | 7,695,386 | 58.58% | 194.7 | 1091.5 |
| zlib L9 | 7,786,253 | 59.27% | 7.0 | 310.0 |
| zlib L6 | 7,814,687 | 59.49% | 20.1 | 300.5 |
| brotli L1 | 7,855,241 | 59.80% | 267.8 | 297.8 |
| zstd L1 | 7,869,981 | 59.91% | 444.6 | 1384.5 |
| lizard L49 | 8,041,060 | 61.21% | 5.7 | 1560.3 |
| zlib L1 | 8,094,068 | 61.62% | 52.4 | 299.7 |
| lizard L45 | 8,253,375 | 62.83% | 16.2 | 1565.7 |
| lizard L40 | 8,481,600 | 64.57% | 336.8 | 1521.6 |
| lz5 HC L15 | 8,603,507 | 65.49% | 2.1 | 993.6 |
| lz5 HC L12 | 8,720,302 | 66.38% | 12.5 | 1060.3 |
| lizard L30 | 8,756,123 | 66.66% | 456.3 | 1942.7 |
| misa77 L3 | 8,867,044 | 67.50% | 10.3 | 5594.6 |
| lz5 HC L9 | 8,900,815 | 67.76% | 23.8 | 1159.9 |
| **lz6 1.6.3-pre HC -15** | 8,996,054 | 68.48% | 11.2 | 1681.3 |
| **lz6 1.6.3-pre HC -12** | 8,996,268 | 68.49% | 11.9 | 1674.1 |
| lz5 HC L6 | 9,066,986 | 69.02% | 42.5 | 1499.5 |
| misa77 L4 | 9,070,575 | 69.05% | 7.7 | 3633.5 |
| **lz6 1.6.3-pre HC -6** | 9,190,747 | 69.97% | 43.9 | 1604.2 |
| misa77 L2 | 9,287,323 | 70.70% | 39.0 | 6670.0 |
| lz5 | 9,469,498 | 72.09% | 327.0 | 1260.2 |
| misa77 L0 | 9,554,603 | 72.73% | 201.8 | 7371.2 |
| misa77 L1 | 9,628,088 | 73.29% | 52.3 | 7883.6 |
| lizard L20 | 9,761,531 | 74.31% | 458.2 | 2519.4 |
| lizard L10 | 9,926,082 | 75.56% | 634.2 | 4760.5 |
| lz4 | 9,938,605 | 75.66% | 791.7 | 5125.9 |
| misa77 L-1 | 10,041,718 | 76.44% | 318.3 | 7517.8 |
| lz5 HC L1 | 10,441,177 | 79.48% | 540.9 | 2548.0 |


## Silesia (212 MB, per-file)

### Silesia (212 MB, per-file)

| codec | csize | ratio | enc MB/s | dec MB/s |
|---|---:|---:|---:|---:|
| xz L9 | 48,795,480 | 23.02% | 2.5 | 109.8 |
| brotli L11 | 50,328,370 | 23.75% | 0.5 | 353.6 |
| zstd L19 | 52,891,946 | 24.95% | 2.7 | 747.6 |
| **lz6 1.6.3-pre seq -15** | 55,922,690 | 26.39% | 2.1 | 273.7 |
| **lz6 1.6.3-pre seq -11** | 56,416,447 | 26.62% | 3.7 | 271.8 |
| xz L1 | 58,716,448 | 27.71% | 16.1 | 92.9 |
| lizard L49 | 60,667,327 | 28.62% | 1.8 | 1153.1 |
| **lz6 1.6.3-pre seq -9** | 61,481,486 | 29.01% | 16.4 | 239.0 |
| zstd L5 | 62,751,501 | 29.60% | 99.4 | 773.3 |
| lz5 HC L15 | 65,595,195 | 30.95% | 2.1 | 711.2 |
| **lz6 1.6.3-pre seq -5** | 67,354,906 | 31.78% | 32.1 | 216.9 |
| **lz6 1.6.3-pre seq -3** | 67,577,295 | 31.89% | 34.7 | 230.4 |
| **lz6 1.6.3-pre seq -2** | 67,635,530 | 31.91% | 37.2 | 255.0 |
| zlib L6 | 68,220,487 | 32.19% | 27.1 | 326.4 |
| **lz6 1.6.3-pre seq -1** | 70,874,587 | 33.44% | 40.8 | 260.1 |
| zstd L1 | 73,229,468 | 34.55% | 351.0 | 1179.0 |
| brotli L1 | 73,429,961 | 34.65% | 202.7 | 317.6 |
| zlib L1 | 77,246,811 | 36.45% | 84.0 | 303.3 |
| misa77 L3 | 80,028,169 | 37.76% | 10.5 | 4452.1 |
| lz5 HC L6 | 80,576,517 | 38.02% | 47.6 | 1143.7 |
| **lz6 1.6.3-pre HC -15** | 81,308,554 | 38.37% | 5.7 | 1282.7 |
| lizard L30 | 85,765,250 | 40.47% | 322.4 | 1217.2 |
| lz5 | 88,218,423 | 41.62% | 231.0 | 693.4 |
| misa77 L1 | 90,385,225 | 42.65% | 49.4 | 4923.1 |
| misa77 L-1 | 99,073,179 | 46.75% | 276.3 | 4608.6 |
| lz4 | 100,880,147 | 47.60% | 546.1 | 3611.5 |
| lz5 HC L1 | 113,525,877 | 53.56% | 510.1 | 1645.5 |

### Silesia per-file ratio

| file | orig | lz6 L15 | lz6 L2 | zstd -19 | xz -9 | brotli -11 | lizard -49 | lz5 HC15 | lz4 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| dickens | 9.7 MB | 30.19% | 37.44% | 27.96% | 27.77% | 27.99% | 33.00% | 36.87% | 63.07% |
| mozilla | 48.8 MB | 31.74% | 38.04% | 29.41% | 26.11% | 27.48% | 34.31% | 35.79% | 51.61% |
| mr | 9.5 MB | 33.40% | 33.87% | 31.16% | 27.58% | 28.34% | 34.20% | 37.92% | 54.57% |
| nci | 32.0 MB | 5.79% | 8.51% | 4.96% | 5.18% | 4.82% | 5.92% | 7.26% | 16.49% |
| ooffice | 5.9 MB | 45.38% | 56.97% | 42.18% | 39.45% | 40.31% | 50.21% | 49.35% | 70.53% |
| osdb | 9.6 MB | 31.66% | 34.76% | 30.74% | 28.26% | 28.01% | 33.78% | 37.30% | 52.12% |
| reymont | 6.3 MB | 22.29% | 31.31% | 20.35% | 19.87% | 20.15% | 23.34% | 27.09% | 48.00% |
| samba | 20.6 MB | 20.11% | 25.36% | 18.04% | 17.42% | 17.69% | 20.90% | 22.43% | 35.72% |
| sao | 6.9 MB | 71.46% | 79.75% | 68.95% | 60.88% | 63.29% | 73.89% | 78.09% | 93.63% |
| webster | 39.5 MB | 21.69% | 29.51% | 20.93% | 20.23% | 21.20% | 24.64% | 27.00% | 48.58% |
| x-ray | 8.1 MB | 56.73% | 56.73% | 60.53% | 52.98% | 55.30% | 66.91% | 75.46% | 99.01% |
| xml | 5.1 MB | 9.78% | 13.32% | 8.47% | 8.48% | 8.05% | 9.98% | 11.23% | 22.96% |

## Findings

**lz6 owns AIT.** L15 is 46.64% against brotli -11's 53.60% - a 7.0 point gap
- while decoding at 234 MB/s (brotli -11: 223 MB/s, xz -9: 67 MB/s). Against
zstd -19 it is 9.3 points smaller for a 4.7x slower decode. Since the repcode
merge (see below) the whole level curve moved down 1.6 points at unchanged
decode speed.

**On Silesia the gap is now within 1.5 points of zstd -19.** L15 is 26.39% @
274 MB/s versus zstd -19's 24.95% @ 748 MB/s and brotli -11's 23.75% @ 354
MB/s. Before the repcode merge the deficit was 2.96 points; it is now 1.44.
zstd -19 and brotli -11 still beat lz6 on both axes, and lizard -49 is 2.2
points behind at 4.2x the decode - but "2.2 points behind at 4x the decode"
is a much weaker competitor claim than it was.

**Per file, lz6 L15 now wins only x-ray** (56.73% vs zstd's 60.53%), but it is
within a point on webster (21.69 vs 20.93) and osdb (31.66 vs 30.74), and
within 2.2 on mr and dickens. The remaining big losses are mozilla (31.74 vs
29.41, xz 26.11), sao (71.46 vs 68.95) and ooffice (45.38 vs 42.18). Those
are the binary/structured cases where the literal coder still goes raw.

**The repcode merge - why it worked.** Instrumenting the encoder showed
offsets dominate: 42-82% of the output (dickens L15: 2.75 MB of 3.36 MB),
with only 12% of sequences using a repcode - yet every sequence paid a fixed
2-bit rep field. Folding the repcode into the offset FSE alphabet codes the
joint (bucket, repcode) distribution instead, saving ~1.4 bits per sequence
on text (measured: Silesia tar L15 62,437,010 -> 58,693,799, -6.0%; AIT
5,367,989 -> 5,194,253, -3.2%) and removing a bitfield read, so decode got
~6% faster as well. It also breaks the seq block format - see NEWS.

**Decode speed remains lz6's structural weakness vs zstd/lizard.** lz6's
230-280 MB/s band sits 4-5x below zstd (1000-1400) and lizard (1200-1900),
and 20x below misa77 (4000-7800, at much worse ratio). Verify+copy dwarfs the
entropy decode here; that is the price of the 32MB window and the context
models.

**Level redundancy is still there.** L6 == L7 and L4 == L5 byte-identical on
AIT, and L11-L15 span 0.03 points (46.64-46.67%) while encode cost ranges
from 8.1 down to 1.9 MB/s. L12 could be dropped with no ratio loss; L13/L14
are effectively L15.

**Not benchmarked here:** brotli/xz are different families with no
fast-decode pretension, but brotli -11 out-decoding lz6 while also being
smaller is still the most uncomfortable line in the Silesia table. Closing it
means fixing mozilla-class literals.
