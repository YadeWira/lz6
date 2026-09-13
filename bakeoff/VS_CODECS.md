# lz6 vs the field - one harness head-to-head

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
   mixed stream into LZ6S3 segments; on the Silesia tar that lands at 28.34%
   (L15) vs 27.91% here, i.e. per-file separation is still the ideal and
   segmentation only recovers part of it.
2. Encode speeds for the heavy codecs are single-iteration-ish at these
   sizes; treat them as order-of-magnitude, not precise.

## AIT A-H (16 MB)

### AIT A-H (16 MB)

| codec | csize | ratio | enc MB/s | dec MB/s |
|---|---:|---:|---:|---:|
| **lz6 seq L14** | 5,367,954 | 48.20% | 4.7 | 233.5 |
| **lz6 seq L13** | 5,367,929 | 48.20% | 5.7 | 232.2 |
| **lz6 seq L15** | 5,367,837 | 48.20% | 1.9 | 233.2 |
| **lz6 seq L12** | 5,371,497 | 48.24% | 8.2 | 231.7 |
| **lz6 seq L11** | 5,371,595 | 48.24% | 8.2 | 232.2 |
| **lz6 seq L10** | 5,380,504 | 48.32% | 17.9 | 236.7 |
| **lz6 seq L9** | 5,402,837 | 48.52% | 18.8 | 232.2 |
| **lz6 seq L6** | 5,454,051 | 48.98% | 24.9 | 226.9 |
| **lz6 seq L7** | 5,454,051 | 48.98% | 25.0 | 226.9 |
| **lz6 seq L8** | 5,454,347 | 48.98% | 24.6 | 226.9 |
| **lz6 seq L3** | 5,494,059 | 49.34% | 63.0 | 252.8 |
| **lz6 seq L2** | 5,496,856 | 49.36% | 79.9 | 280.9 |
| **lz6 seq L4** | 5,571,456 | 50.03% | 47.3 | 230.5 |
| **lz6 seq L5** | 5,571,456 | 50.03% | 47.5 | 230.3 |
| **lz6 seq L1** | 5,613,377 | 50.41% | 90.6 | 280.0 |
| brotli L11 | 7,041,245 | 53.60% | 0.6 | 222.7 |
| xz L6 | 7,114,024 | 54.16% | 3.9 | 69.4 |
| xz L9 | 7,114,024 | 54.16% | 3.4 | 68.1 |
| zstd L19 | 7,351,946 | 55.97% | 5.5 | 1078.8 |
| xz L3 | 7,379,360 | 56.18% | 6.8 | 67.7 |
| xz L1 | 7,435,760 | 56.60% | 9.4 | 65.6 |
| brotli L9 | 7,468,536 | 56.86% | 13.7 | 313.0 |
| brotli L6 | 7,489,339 | 57.01% | 33.1 | 314.1 |
| zstd L15 | 7,530,938 | 57.33% | 13.7 | 1005.9 |
| zstd L12 | 7,552,509 | 57.49% | 37.0 | 1003.6 |
| zstd L9 | 7,578,176 | 57.69% | 67.2 | 1013.8 |
| brotli L4 | 7,591,522 | 57.79% | 81.7 | 337.1 |
| zstd L5 | 7,619,521 | 58.00% | 129.3 | 983.7 |
| zstd L3 | 7,695,386 | 58.58% | 188.4 | 1062.2 |
| zlib L9 | 7,786,253 | 59.27% | 7.3 | 313.6 |
| zlib L6 | 7,814,687 | 59.49% | 21.0 | 306.1 |
| brotli L1 | 7,855,241 | 59.80% | 264.1 | 297.9 |
| zstd L1 | 7,869,981 | 59.91% | 436.9 | 1378.0 |
| lizard L49 | 8,041,060 | 61.21% | 5.8 | 1552.5 |
| zlib L1 | 8,094,068 | 61.62% | 55.2 | 305.7 |
| lizard L45 | 8,253,375 | 62.83% | 17.5 | 1560.1 |
| lizard L40 | 8,481,600 | 64.57% | 334.4 | 1516.2 |
| lz5 HC L15 | 8,603,507 | 65.49% | 2.2 | 984.7 |
| lz5 HC L12 | 8,720,302 | 66.38% | 12.1 | 1058.3 |
| lizard L30 | 8,756,123 | 66.66% | 458.2 | 1935.1 |
| misa77 L3 | 8,867,044 | 67.50% | 10.2 | 5538.2 |
| lz5 HC L9 | 8,900,815 | 67.76% | 22.2 | 1154.4 |
| **lz6 HC L15** | 8,996,054 | 68.48% | 11.2 | 1673.2 |
| **lz6 HC L12** | 8,996,268 | 68.49% | 11.8 | 1672.4 |
| lz5 HC L6 | 9,066,986 | 69.02% | 42.4 | 1516.0 |
| misa77 L4 | 9,070,575 | 69.05% | 7.6 | 3461.8 |
| **lz6 HC L6** | 9,190,747 | 69.97% | 43.5 | 1610.3 |
| misa77 L2 | 9,287,323 | 70.70% | 38.6 | 6586.2 |
| lz5 | 9,469,498 | 72.09% | 324.8 | 1260.5 |
| misa77 L0 | 9,554,603 | 72.73% | 214.9 | 7277.0 |
| misa77 L1 | 9,628,088 | 73.29% | 50.7 | 7788.5 |
| lizard L20 | 9,761,531 | 74.31% | 452.2 | 2505.0 |
| lizard L10 | 9,926,082 | 75.56% | 636.3 | 4733.3 |
| lz4 | 9,938,605 | 75.66% | 791.9 | 5099.8 |
| misa77 L-1 | 10,041,718 | 76.44% | 332.6 | 7415.8 |
| lz5 HC L1 | 10,441,177 | 79.48% | 538.5 | 2521.9 |


## Silesia (212 MB, per-file)

### Silesia (212 MB, per-file)

| codec | csize | ratio | enc MB/s | dec MB/s |
|---|---:|---:|---:|---:|
| xz L9 | 48,795,480 | 23.02% | 2.5 | 112.7 |
| brotli L11 | 50,328,370 | 23.75% | 0.5 | 365.0 |
| zstd L19 | 52,891,946 | 24.95% | 2.8 | 767.8 |
| xz L1 | 58,716,448 | 27.71% | 15.9 | 96.7 |
| **lz6 seq L15** | 59,151,273 | 27.91% | 2.2 | 275.9 |
| **lz6 seq L11** | 59,674,236 | 28.16% | 3.8 | 270.9 |
| lizard L49 | 60,667,327 | 28.62% | 1.8 | 1186.0 |
| zstd L5 | 62,751,501 | 29.60% | 101.6 | 788.8 |
| **lz6 seq L9** | 65,027,809 | 30.68% | 16.3 | 240.5 |
| lz5 HC L15 | 65,595,195 | 30.95% | 2.1 | 750.2 |
| zlib L6 | 68,220,487 | 32.19% | 27.7 | 333.7 |
| **lz6 seq L2** | 70,645,854 | 33.33% | 37.3 | 257.3 |
| **lz6 seq L5** | 71,097,848 | 33.55% | 31.5 | 217.5 |
| **lz6 seq L3** | 71,154,016 | 33.57% | 34.5 | 236.7 |
| zstd L1 | 73,229,468 | 34.55% | 353.1 | 1196.1 |
| brotli L1 | 73,429,961 | 34.65% | 208.4 | 337.2 |
| **lz6 seq L1** | 73,649,153 | 34.75% | 40.7 | 267.7 |
| zlib L1 | 77,246,811 | 36.45% | 86.3 | 309.4 |
| misa77 L3 | 80,028,169 | 37.76% | 11.0 | 4565.4 |
| lz5 HC L6 | 80,576,517 | 38.02% | 46.7 | 1172.4 |
| **lz6 HC L15** | 81,308,486 | 38.37% | 5.5 | 1297.4 |
| lizard L30 | 85,765,250 | 40.47% | 326.0 | 1234.0 |
| lz5 | 88,218,423 | 41.62% | 214.5 | 696.5 |
| misa77 L1 | 90,385,225 | 42.65% | 50.8 | 5085.4 |
| misa77 L-1 | 99,073,179 | 46.75% | 282.5 | 4590.5 |
| lz4 | 100,880,147 | 47.60% | 539.9 | 3508.1 |
| lz5 HC L1 | 113,525,877 | 53.56% | 510.5 | 1649.3 |

### Silesia per-file ratio (the honest picture)

| file | orig | lz6 L15 | zstd -19 | xz -9 | brotli -11 | lizard -49 | lz5 HC15 | lz4 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| dickens | 9.7 MB | 32.95% | 27.96% | 27.77% | 27.99% | 33.00% | 36.87% | 63.07% |
| mozilla | 48.8 MB | 33.83% | 29.41% | 26.11% | 27.48% | 34.31% | 35.79% | 51.61% |
| mr | 9.5 MB | 33.67% | 31.16% | 27.58% | 28.34% | 34.20% | 37.92% | 54.57% |
| nci | 32.0 MB | 6.19% | 4.96% | 5.18% | 4.82% | 5.92% | 7.26% | 16.49% |
| ooffice | 5.9 MB | 48.34% | 42.18% | 39.45% | 40.31% | 50.21% | 49.35% | 70.53% |
| osdb | 9.6 MB | 32.93% | 30.74% | 28.26% | 28.01% | 33.78% | 37.30% | 52.12% |
| reymont | 6.3 MB | 24.19% | 20.35% | 19.87% | 20.15% | 23.34% | 27.09% | 48.00% |
| samba | 20.6 MB | 21.44% | 18.04% | 17.42% | 17.69% | 20.90% | 22.43% | 35.72% |
| sao | 6.9 MB | 73.49% | 68.95% | 60.88% | 63.29% | 73.89% | 78.09% | 93.63% |
| webster | 39.5 MB | 23.49% | 20.93% | 20.23% | 21.20% | 24.64% | 27.00% | 48.58% |
| x-ray | 8.1 MB | 57.42% | 60.53% | 52.98% | 55.30% | 66.91% | 75.46% | 99.01% |
| xml | 5.1 MB | 10.54% | 8.47% | 8.48% | 8.05% | 9.98% | 11.23% | 22.96% |

## Findings

**lz6 owns AIT, and only AIT.** On AIT L15 (48.20%) the nearest competitor is
brotli -11 at 53.60% - a 5.4 point gap - and lz6 does it while decoding at
233 MB/s (brotli -11: 223 MB/s, xz -9: 68 MB/s). Against zstd -19 it is 7.8
points smaller for a 4.6x slower decode. That is the whole proposition: the
best ratio in the ~250 MB/s decode band, by a lot.

**On Silesia that proposition does not hold.** lz6 L15 is 27.91% @ 276 MB/s,
and both xz -9 (23.02%), brotli -11 (23.75%) and zstd -19 (24.95%) are
smaller. Worse, zstd -19 decodes at 768 MB/s and brotli -11 at 365 MB/s, so
those two beat lz6 on **both** axes at comparable encode cost (zstd -19: 2.8
MB/s encode vs lz6 L15: 2.2). Per-file, lz6 L15 beats zstd -19 on exactly one
Silesia file, x-ray (57.42% vs 60.53%).

**The loss is concentrated in text and source, not in the numeric/DB data.**
The biggest gaps are mozilla (33.83% vs zstd 29.41%, xz 26.11%), dickens
(32.95% vs 27.96%), samba (21.44% vs 18.04%) and webster (23.49% vs 20.93%).
On x-ray it wins outright. That is consistent with the AIT result (AIT is
float and DB heavy) and with the known literal-coding weakness: mozilla/nci
literals still go raw.

**lz6 vs lz5 is not a contest.** lz5 HC L15: 65.49% on AIT and 30.95% on
Silesia, against lz6 48.20% / 27.91%. The successor is 17.3 and 3.0 points
better respectively, at comparable decode speed.

**lizard -49 nearly matches lz6 L15 on Silesia with 4.3x the decode.** 28.62%
@ 1186 MB/s against lz6's 27.91% @ 276 MB/s - a 0.7 point ratio edge for lz6
bought with a 4.3x slower decode. On AIT the same comparison is 48.20% vs
61.21%, so lizard's near-parity is specific to the Silesia mix. Still, a 0.7
point margin at 4x the decode cost is a thin claim to the "max ratio with fast
decode" objective on mixed data.

**Decode speed is lz6's structural weakness vs zstd/lizard.** lz6's 230-280
MB/s band sits 4-5x below zstd (1000-1400) and lizard (1200-1900), and 20x
below misa77 (4000-7800, at much worse ratio). Verify+copy dwarfs the entropy
decode here; that is the price of the 32MB window and the context models.

**Level redundancy worth cleaning up.** L6 == L7 and L4 == L5 byte-identical
on AIT, and L11-L15 span just 0.04 points (48.20-48.24%) while encode cost
ranges from 8.2 down to 1.9 MB/s. L12 could be dropped with no ratio loss, and
L13/L14 are effectively L15. The level table is leaving free decode-neutral
ratio on the table by spending levels that do nothing.

**Not benchmarked here:** brotli/xz feel like different tools (different
families, no fast-decode pretension), but brotli -11 out-decoding lz6 while
also being smaller is the single most uncomfortable line in the Silesia table.
Fixing mozilla-class literals is where that gap lives.
