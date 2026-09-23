# lz6 vs the field - one harness head-to-head

All numbers below come from a single harness: **lzbench 2.3.1**, one thread
(`-t1,1`), memory-to-memory per file, on an Intel Xeon E5-2697A v4 @ 2.60GHz.
lz6 and lz5 are integrated as lzbench plugins (see
[lzbench_lz6.patch](lzbench_lz6.patch)) so they are timed by the same loop,
with the same buffers and the same round-trip verification as lz4, lizard,
zstd, misa77, brotli, xz and zlib.

Last full sweep: 2026-09-23, master `721fa64` (seq v2: tANS sequence
symbols in one backward bitstream). fastlzma2 / uf-lzma2 / memlz rows are
from the 2026-09-22 run on the same machine and settings. Added then, on the same
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
   (L15, before seq v2) vs 26.39% here at the time, i.e. per-file separation is still the ideal and
   segmentation only recovers part of it.
2. Encode speeds for the heavy codecs are single-iteration-ish at these
   sizes; treat them as order-of-magnitude, not precise.

## AIT A-H (16 MB)

### AIT A-H (16 MB)

| codec | csize | ratio | enc MB/s | dec MB/s |
|---|---:|---:|---:|---:|
| **lz6 seq -15** | 5,193,670 | 39.54% | 2.1 | 404.6 |
| **lz6 seq -13** | 5,193,758 | 39.54% | 6.4 | 404.2 |
| **lz6 seq -14** | 5,193,779 | 39.54% | 5.3 | 404.3 |
| **lz6 seq -12** | 5,196,924 | 39.56% | 9.2 | 396.7 |
| **lz6 seq -11** | 5,197,028 | 39.56% | 9.3 | 394.3 |
| **lz6 seq -10** | 5,220,334 | 39.74% | 20.5 | 413.1 |
| **lz6 seq -9** | 5,238,723 | 39.88% | 21.0 | 409.7 |
| **lz6 seq -8** | 5,282,943 | 40.22% | 28.3 | 402.2 |
| **lz6 seq -6** | 5,282,945 | 40.22% | 28.7 | 402.7 |
| **lz6 seq -7** | 5,282,945 | 40.22% | 28.8 | 402.4 |
| **lz6 seq -3** | 5,373,318 | 40.90% | 75.0 | 424.9 |
| **lz6 seq -4** | 5,401,410 | 41.12% | 56.5 | 400.7 |
| **lz6 seq -5** | 5,401,410 | 41.12% | 56.1 | 409.3 |
| **lz6 seq -2** | 5,429,532 | 41.33% | 94.5 | 435.9 |
| **lz6 seq -1** | 5,557,735 | 42.31% | 106.4 | 446.0 |
| brotli L11 | 7,041,245 | 53.60% | 0.6 | 222.5 |
| fastlzma2 L10 | 7,067,576 | 53.80% | 6.2 | 47.0 |
| uf-lzma2 L10 (asm dec) | 7,067,576 | 53.80% | 6.2 | 63.9 |
| fastlzma2 L5 | 7,086,428 | 53.95% | 8.6 | 46.5 |
| uf-lzma2 L5 (asm dec) | 7,086,428 | 53.95% | 8.6 | 62.8 |
| xz L6 | 7,114,024 | 54.16% | 3.7 | 66.7 |
| xz L9 | 7,114,024 | 54.16% | 2.9 | 63.4 |
| fastlzma2 L1 | 7,330,324 | 55.80% | 16.3 | 49.3 |
| uf-lzma2 L1 (asm dec) | 7,330,324 | 55.80% | 16.4 | 70.4 |
| zstd L19 | 7,351,946 | 55.97% | 5.3 | 1085.3 |
| xz L3 | 7,379,360 | 56.18% | 6.5 | 65.0 |
| xz L1 | 7,435,760 | 56.60% | 8.9 | 63.1 |
| brotli L9 | 7,468,536 | 56.85% | 13.8 | 308.8 |
| brotli L6 | 7,489,339 | 57.01% | 33.4 | 312.0 |
| zstd L15 | 7,530,938 | 57.33% | 13.5 | 1022.9 |
| zstd L12 | 7,552,509 | 57.49% | 36.8 | 1016.1 |
| zstd L9 | 7,578,176 | 57.69% | 70.3 | 1029.5 |
| brotli L4 | 7,591,522 | 57.79% | 80.4 | 327.5 |
| zstd L5 | 7,619,521 | 58.00% | 130.4 | 995.8 |
| zstd L3 | 7,695,386 | 58.58% | 190.6 | 1059.4 |
| zlib L9 | 7,786,253 | 59.27% | 6.4 | 300.2 |
| zlib L6 | 7,814,687 | 59.49% | 20.0 | 287.2 |
| brotli L1 | 7,855,241 | 59.80% | 263.7 | 290.0 |
| zstd L1 | 7,869,981 | 59.91% | 430.6 | 1360.2 |
| lizard L49 | 8,041,060 | 61.21% | 5.3 | 1537.1 |
| zlib L1 | 8,094,068 | 61.62% | 51.7 | 297.1 |
| lizard L45 | 8,253,375 | 62.83% | 16.2 | 1543.8 |
| lizard L40 | 8,481,600 | 64.57% | 333.1 | 1500.5 |
| lz5 HC L15 | 8,603,507 | 65.49% | 2.1 | 985.4 |
| lz5 HC L12 | 8,720,302 | 66.38% | 11.0 | 1034.5 |
| lizard L30 | 8,756,123 | 66.66% | 456.0 | 1919.1 |
| misa77 L3 | 8,867,044 | 67.50% | 9.9 | 5546.5 |
| lz5 HC L9 | 8,900,815 | 67.76% | 20.6 | 1160.2 |
| **lz6 HC -15** | 8,996,054 | 68.48% | 10.5 | 1648.1 |
| **lz6 HC -12** | 8,996,268 | 68.48% | 11.3 | 1637.3 |
| lz5 HC L6 | 9,066,986 | 69.02% | 42.4 | 1502.1 |
| misa77 L4 | 9,070,575 | 69.05% | 7.4 | 3565.1 |
| **lz6 HC -6** | 9,190,747 | 69.96% | 43.2 | 1579.2 |
| misa77 L2 | 9,287,323 | 70.70% | 38.5 | 6609.2 |
| lz5 | 9,469,498 | 72.09% | 325.9 | 1278.9 |
| misa77 L0 | 9,554,603 | 72.73% | 203.0 | 7349.8 |
| misa77 L1 | 9,628,088 | 73.29% | 50.8 | 7826.5 |
| lizard L20 | 9,761,531 | 74.31% | 449.4 | 2475.0 |
| lizard L10 | 9,926,082 | 75.56% | 622.0 | 4711.1 |
| lz4 | 9,938,605 | 75.66% | 786.1 | 5119.0 |
| misa77 L-1 | 10,041,718 | 76.44% | 327.1 | 7490.5 |
| lz5 HC L1 | 10,441,177 | 79.48% | 537.9 | 2540.5 |
| memlz | 10,724,346 | 81.64% | 1872.5 | 1712.7 |

## Silesia (212 MB, per-file)

### Silesia (212 MB, per-file)

| codec | csize | ratio | enc MB/s | dec MB/s |
|---|---:|---:|---:|---:|
| fastlzma2 L10 | 48,673,262 | 22.97% | 3.3 | 81.0 |
| uf-lzma2 L10 (asm dec) | 48,673,262 | 22.97% | 3.3 | 110.5 |
| xz L9 | 48,795,480 | 23.02% | 2.5 | 103.7 |
| brotli L11 | 50,328,370 | 23.75% | 0.5 | 365.6 |
| fastlzma2 L5 | 51,124,925 | 24.12% | 5.9 | 77.9 |
| uf-lzma2 L5 (asm dec) | 51,124,925 | 24.12% | 6.1 | 107.0 |
| zstd L19 | 52,891,946 | 24.96% | 2.7 | 777.8 |
| **lz6 seq -15** | 56,210,308 | 26.52% | 2.2 | 492.5 |
| **lz6 seq -11** | 56,708,234 | 26.76% | 3.7 | 488.4 |
| xz L1 | 58,716,448 | 27.70% | 15.5 | 94.9 |
| fastlzma2 L1 | 58,993,508 | 27.84% | 17.8 | 65.1 |
| uf-lzma2 L1 (asm dec) | 58,993,508 | 27.84% | 18.1 | 96.4 |
| lizard L49 | 60,667,327 | 28.62% | 1.8 | 1157.9 |
| **lz6 seq -9** | 61,914,243 | 29.21% | 17.2 | 450.9 |
| zstd L5 | 62,751,501 | 29.61% | 99.1 | 778.6 |
| lz5 HC L15 | 65,595,195 | 30.95% | 2.1 | 739.5 |
| **lz6 seq -5** | 67,669,430 | 31.93% | 33.7 | 399.9 |
| **lz6 seq -3** | 67,818,086 | 32.00% | 35.5 | 430.6 |
| **lz6 seq -2** | 67,826,536 | 32.00% | 39.3 | 446.4 |
| zlib L6 | 68,220,487 | 32.19% | 26.3 | 330.8 |
| **lz6 seq -1** | 71,030,356 | 33.51% | 41.0 | 438.4 |
| zstd L1 | 73,229,468 | 34.55% | 347.8 | 1172.6 |
| brotli L1 | 73,429,961 | 34.65% | 207.9 | 328.6 |
| zlib L1 | 77,246,811 | 36.45% | 75.7 | 288.6 |
| misa77 L3 | 80,028,169 | 37.76% | 10.5 | 4537.5 |
| lz5 HC L6 | 80,576,517 | 38.02% | 47.2 | 1153.0 |
| **lz6 HC -15** | 81,308,534 | 38.36% | 5.5 | 1274.4 |
| lizard L30 | 85,765,250 | 40.47% | 323.3 | 1219.9 |
| lz5 | 88,218,423 | 41.62% | 231.9 | 690.7 |
| misa77 L1 | 90,385,225 | 42.65% | 50.6 | 5189.8 |
| misa77 L-1 | 99,073,179 | 46.75% | 264.1 | 4783.9 |
| lz4 | 100,880,147 | 47.60% | 542.9 | 3545.6 |
| lz5 HC L1 | 113,525,877 | 53.57% | 508.6 | 1637.3 |
| memlz | 126,970,186 | 59.91% | 963.2 | 887.8 |

### Silesia per-file ratio

| file | orig | lz6 L15 | lz6 L2 | zstd -19 | xz -9 | brotli -11 | lizard -49 | lz5 HC15 | lz4 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| dickens | 9.7 MB | 30.21% | 37.47% | 27.96% | 27.77% | 27.99% | 33.00% | 36.87% | 63.07% |
| mozilla | 48.8 MB | 32.09% | 38.19% | 29.41% | 26.11% | 27.48% | 34.31% | 35.79% | 51.61% |
| mr | 9.5 MB | 33.41% | 33.88% | 31.16% | 27.58% | 28.34% | 34.20% | 37.92% | 54.57% |
| nci | 32.0 MB | 5.87% | 8.71% | 4.96% | 5.18% | 4.82% | 5.92% | 7.26% | 16.49% |
| ooffice | 5.9 MB | 45.39% | 56.98% | 42.18% | 39.45% | 40.31% | 50.21% | 49.35% | 70.53% |
| osdb | 9.6 MB | 31.69% | 34.76% | 30.74% | 28.26% | 28.01% | 33.78% | 37.30% | 52.12% |
| reymont | 6.3 MB | 22.32% | 31.32% | 20.35% | 19.87% | 20.15% | 23.34% | 27.09% | 48.00% |
| samba | 20.6 MB | 20.12% | 25.37% | 18.04% | 17.42% | 17.69% | 20.90% | 22.43% | 35.72% |
| sao | 6.9 MB | 72.01% | 79.82% | 68.95% | 60.88% | 63.29% | 73.89% | 78.09% | 93.63% |
| webster | 39.5 MB | 21.72% | 29.54% | 20.93% | 20.23% | 21.20% | 24.64% | 27.00% | 48.58% |
| x-ray | 8.1 MB | 57.02% | 57.02% | 60.53% | 52.98% | 55.30% | 66.91% | 75.46% | 99.01% |
| xml | 5.1 MB | 9.77% | 13.30% | 8.47% | 8.48% | 8.05% | 9.98% | 11.23% | 22.96% |

## Findings

**AIT: the lead is file D.** L15 is 39.54% on the 8 files against brotli
-11's 53.60% and zstd -19's 55.97%, decoding at 405 MB/s. Almost all of that
comes from one file: D is glibc `random()` output, which lz6 regenerates
from its seed (2,000,000 -> 7 bytes) while every other codec stores it raw.
Without D, lz6 -15 is 46.64% and sits **behind** the LZMA/brotli class:

| codec | 8 files | without D | A | B | C | E | F | G | H |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **lz6 -15** | **39.54%** | 46.64% | 57.77% | 19.27% | 28.90% | 79.76% | 79.43% | 29.35% | 41.02% |
| brotli -11 | 53.60% | 45.27% | 50.85% | 17.41% | 24.92% | 84.75% | 79.28% | 30.97% | 36.26% |
| uf-lzma2 -10 | 53.80% | 45.50% | 51.87% | 17.78% | 24.78% | 85.52% | 82.53% | 28.53% | 35.97% |
| xz -9 | 54.16% | 45.92% | 53.34% | 17.76% | 24.72% | 85.84% | 82.38% | 29.71% | 35.87% |
| zstd -19 | 55.97% | 48.06% | 51.77% | 18.06% | 26.66% | 88.52% | 87.83% | 31.61% | 38.49% |

Per file, lz6 -15 wins E and F (the byte-plane transform) and is close on
G; on A, B, C and H it loses even to zstd -19. (Until 2026-09-22 these
tables showed lz6 at 46.66% against the other codecs' 8-file totals: the
parser rebuilt each file's size from lzbench's rounded ratio, and D's 0.00
ratio dropped it from lz6's denominator only. Fixed in parse_lzbench.py,
which now uses the real file sizes; reported by uf-lzma2.)

**On Silesia lz6 -15 is 1.6 points behind zstd -19 and out-decodes
brotli -11 by a third.** L15 is 26.52% @ 493 MB/s versus zstd -19's 24.96% @
778 MB/s and brotli -11's 23.75% @ 366 MB/s. zstd -19 still beats lz6 on both
axes, but its decode lead is now 1.6x (it was 2.0x before seq v2). lizard -49
is 2.1 points behind lz6 at 2.35x the decode (1158 MB/s).

**LZMA2 with an assembler decoder.** uf-lzma2 -10 is the best ratio in the
Silesia table (22.97%, xz -9: 23.02%) at 110 MB/s decode, 36-48% faster than
the same streams through fastlzma2's C decoder. That is the ceiling of the
bit-wise range-coder class; lz6 -15 trades 3.6 points of ratio for 4.5x the
decode.

**memlz** is the opposite end: ~0.9 GB/s both ways on Silesia (1.7-1.9 GB/s
on AIT) at 59.91%, a worse ratio than lz4 (47.60%): a compression-speed codec.

**Per file, lz6 L15 wins only x-ray** (57.02% vs zstd's 60.53%), but it is
within a point on webster (21.72 vs 20.93) and osdb (31.69 vs 30.74), and
within 2.3 on mr and dickens. The remaining big losses are mozilla (32.09 vs
29.41, xz 26.11), sao (72.01 vs 68.95) and ooffice (45.39 vs 42.18): the
binary/structured cases, where both the literal coder and seq v2's raw
offset bits cost the most.

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

**Decode speed remains lz6's weak axis vs zstd/lizard.** lz6's 405-500
MB/s band sits 1.6-3x below zstd (778-1342) and lizard (1158-1909), and
10-19x below misa77 (4600-7800, at much worse ratio). The seq v2 decoder is
instruction-bound (IPC ~2.5, ~310 instructions per sequence); see the wiki's
Decoder Internals and Roadmap pages.

**Level redundancy is still there.** L6 == L7 and L4 == L5 byte-identical on
AIT, and L11-L15 span 0.02 points (39.54-39.56%) while encode cost ranges
from 9.3 down to 2.1 MB/s. L12 could be dropped with no ratio loss; L13/L14
are effectively L15. On AIT the fastest-decoding seq level is L1 (446 MB/s).

**Not benchmarked here:** brotli/xz are different families with no
fast-decode pretension. The uncomfortable line is now zstd -19, which is
both smaller and 2x faster to decode on Silesia; closing the ratio part
means fixing mozilla-class literals.
