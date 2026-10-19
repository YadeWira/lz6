# Single thread: where lz6 stands against zstd, and why

> **Level labels (v1.6.12-pre).** This document was written before v1.6.12-pre renumbered the seq levels, and its measurements are not repeated: where it says `lz6 -N` or "level N" for the seq engine with N >= 3, that is `-(N-2)` now (old `-3` = `-1`, old `-15` = `-13`), and the old `-1` and `-2` are `--fast=2` and `--fast=1` (a smaller, slower fast tier now: 32.56% and 30.45% on Silesia). `--hc` levels did not move. The study measured v1.6.9-pre; the decoder it led to is v1.6.11-pre's, and v1.6.12-pre's delta between the two is in [V1612_VS_V1611.md](V1612_VS_V1611.md).

Measured on 2026-10-02 with lz6 v1.6.9-pre (the engine is unchanged in v1.6.10-pre), Intel Xeon E5-2697A v4,
one core pinned, library API with one block per file. The counters are read around the codec call only, with
[stdh.c](stdh.c) (build instructions in its header); the round trip is verified on every decode. Summing the
12 Silesia files with this harness reproduces the lzbench sweep exactly (lz6 -3 28.97%, zstd -5 29.61%,
lz6 -2 31.65%, lz6 -1 33.16%, zstd -1 34.55%). Compressed sizes match lzbench's byte for byte where compared (zstd -1, -3, -5 and lz6 -1, -2, -3). Cycle counts carry about 5% noise (a sibling hyper-thread is
enough); instruction counts are exact.

## Silesia, ratio and cycles per byte (byte-weighted over the 12 files)

| codec | ratio | enc cyc/B | dec cyc/B | | codec | ratio | enc cyc/B | dec cyc/B |
|---|---:|---:|---:|---|---|---:|---:|---:|
| zstd -1 | 34.55% | 10.9 | 3.06 | | **lz6 -1** | 33.16% | 20.2 | 5.72 |
| zstd -2 | 32.71% | 15.2 | 3.93 | | **lz6 -2** | 31.65% | 24.9 | 6.52 |
| zstd -3 | 31.21% | 20.8 | 4.37 | | **lz6 -3** | 28.97% | 72.1 | 6.06 |
| zstd -4 | 30.65% | 21.1 | 5.21 | | **lz6 -4** | 28.25% | 82.4 | 5.65 |
| zstd -5 | 29.61% | 33.8 | 4.56 | | **lz6 -5** | 27.62% | 118.8 | 5.55 |
| zstd -6 | 28.90% | 45.3 | 4.26 | | **lz6 -6** | 27.49% | 137.7 | 5.60 |
| zstd -7 | 28.43% | 50.2 | 4.44 | | **lz6 -8** | 27.08% | 427 | 5.89 |
| zstd -9 | 27.88% | 63.5 | 4.65 | | **lz6 -12** | 25.31% | 1,280 | 5.62 |
| zstd -12 | 27.33% | 129 | 4.79 | | **lz6 -15** | 25.20% | 4,811 | 5.69 |
| zstd -14 | 27.03% | 353 | 4.79 | | | | | |
| zstd -17 | 25.58% | 742 | 4.73 | | | | | |
| zstd -19 | 24.96% | 1,328 | 4.91 | | | | | |

zstd is smaller, cheaper to encode and faster to decode than: lz6 -1 and -2 (zstd -3), lz6 -3 (zstd -6), lz6 -4
(zstd -9), lz6 -6 (zstd -12), lz6 -8 (zstd -14), lz6 -12 and -15 (zstd -19). lz6 -5 against zstd -12: zstd is
smaller and decodes faster, lz6 -5 is 8% cheaper to encode.

Per file (ratio, lz6 minus zstd, lz6 -5 against zstd -12 / lz6 -3 against zstd -6): ooffice -4.73 / -5.37
(x86 filter), x-ray -6.83 / -8.02 (byte planes), sao -0.51 / -1.25; mr +0.33 / -0.38, osdb +0.90 / -0.37;
the others +0.1 to +1.7 (lz6 worse). The advantage is in the transforms.

## Decode: the gap is instruction count, not IPC

Instructions per sequence (instructions per byte times bytes per sequence), by level:

| file | zstd -1 / -5 / -19 | lz6 -1 / -3 / -15 |
|---|---|---|
| xml | 160 / 150 / 157 | 251 / 247 / 253 |
| dickens | 162 / 129 / 130 | 250 / 231 / 232 |
| samba | 160 / 146 / 148 | 291 / 269 / 270 |

lz6 runs 230-290 instructions per sequence whatever the sequence length, zstd 130-160, for about the same
number of sequences, and at a higher IPC (3.1-3.8 against 2.7-3.4). In the hot loop of `decode_normal`
(238 instructions on dickens -3) 71 are loads and stores to the stack (spilled tANS states, table pointers, bit
accumulator, and the 3 stores + 3 loads per sequence of the 16-entry ring between the two stages); 10 are branches.

## Encode

Cycles per byte, lz6 -1 against zstd -1: xml 10.2 / 6.2, dickens 29.9 / 15.0, samba 16.6 / 8.5, ooffice 34.2 / 12.7
(1.4-1.6x the instructions, IPC 1.5-1.9 against 2.1-2.7). Last-level-cache misses per input byte on dickens: lz6 -1
0.029, zstd -1 0.0003; lz6 -3 0.116, zstd -5 0.006: lz6's hash tables are far bigger than zstd's level-1 table.
Profile of lz6 -1: `compress_normal_ex` (parser fused with the tANS writer) 72-75%, kernel 8-10% (page faults on
the fresh buffers of every call), histograms 3-7%, Huffman 3-4%. At -3: the row-hash parser 70%, the entropy stage 13%,
kernel 5.6%.

## Levers measured (on a scratch copy of the library)

- **Fixed cost per sequence in the optimal parser** (levels 8-15; `λ` bits added to every match price): dickens -15
  with λ=8: +1.8% size, -9% decode cycles; samba -12 with λ=16: +3.1% size, -11%; xml -12 with λ=16: +3.4%, -12%.
  Past a point the literals switch to the order-1 mode, which decodes about twice as slowly (IPC ~1): it needs a guard.
- **Dropping matches shorter than K** after the parse is a bad rate (dickens -3, K=6: -21% sequences, -13% decode
  cycles, +6.2% size; K=8: -58% sequences, -29% cycles, +24% size).
- Not measured, only estimated: a rewrite of the decode loop to cut register pressure (-10 to -35% instructions),
  reusing buffers across blocks (kernel share, 5-10% of encode).

## Levels 1-5: sweeps, and what was adopted (2026-10-02)

Whole Silesia, one thread, same counters. Ratios are exact; cycles carry 5-15% noise between runs on this machine, so
only differences measured side by side are trusted. Counters exclude kernel time: page-fault costs only show in wall time.

- **L1 and L2 are polished within their parser.** 36 settings of `fast1` (hash 2^13..2^20, 5-7 hashed bytes, window
  2^20 or 2^25): none dominates L1; 7 hashed bytes at L2 gives 31.63% against 31.65% on Silesia but +0.10 points on AIT.
- **The row finder (L3-L6) has a floor of ~48 cycles/byte** whatever its candidates (1-8) and table (2^16..2^22 slots):
  31.60% at 48.2; fast1 never costs more than ~24. Nothing existed between 24 and 48.
- **A two-table parser (`fast2`: long table over 8 bytes, short over 5, long candidate tried one byte ahead)** fills it:
  30.43% at 34.3 cycles/byte (2^21 / 2^18 entries, window 2^25), 30.64% at 29.5, 30.81% at 27.7; smaller tables do not
  make it cheaper (floor ~25-28). Placed at L3 it compresses worse than the old L3 (30.45% against 28.97%), so it was not
  adopted (branch parked-fast2-ladder). zstd -4 gets 30.65% at ~21 cycles/byte.
- **L3 with a 6-byte hash:** 28.87% against 28.97% (AIT 39.62% against 39.73%) at the same cost: adopted.
- **Order-1 literals at levels 4-7:** with them off the output is byte-identical on Silesia and AIT and the encode ~5%
  cheaper. They were the reason decode time jumped when a parser change gave the literals more weight (L4 with a 7-byte
  hash: 13.5 cycles/byte instead of 5.9). Now tried from level 8.
- **Where a level-1..3 encode spends its time** (dickens, parsers un-inlined): parser 49/58/60%, entropy stage
  (`compress_normal_ex`) 24/21/20%, kernel 9/5/6%, byte-plane histogram 7/6/6% (the stride-2 division, now fixed).
  Software prefetch of the next hash slot: +-1.5%, noise.


## Decoder instruction diet (2026-10-09)

The section above ("Decode: the gap is instruction count, not IPC") ended with the decoder at 232-278 instructions per
sequence at level 3 (xml / dickens / samba: 253 / 232 / 278; zstd 121-136) and the question of what to do about it. Compiler
flags were ruled out first, then the decoder was rewritten (details and per-step numbers in NEWS, v1.6.11-pre).

**No build-flag lever** (decode, gcc -O2 = 100%, dickens/samba/xml at L1 and L3, 3 interleaved rounds, medians, CPU 44, same
instructions and compressed bytes): gcc -O3 +2.4%, -march=native 0%, gcc PGO +2%, clang -O2 +8%, clang -O3 +7%, clang PGO +3%.
Encode only: -march=native -6% (gcc) / -8% (clang), gcc -O3 -0.6%, gcc PGO -0.4%, clang PGO -2.7%. -march=native is not portable
(the release binaries are static and must run on any x86-64), so none of it was adopted.

**Result** (decode only, one pinned core, 12 interleaved rounds, 20 files = Silesia + AIT A-H, size-weighted cycles per byte
against v1.6.10-pre; the compressed streams are byte-identical):

| level | v1.6.10-pre | SEQ_AHEAD 16 | SEQ_AHEAD 64 (shipped) |
|---|---:|---:|---:|
| 1 | 5.669 | -36.5% | -38.2% |
| 3 | 5.878 | -34.4% | -40.9% |
| 8 | 5.717 | -35.1% | -38.1% |
| 15 | 5.411 | -34.6% | -37.8% |

Every file is faster at every level (smallest gain AIT G, -18%). Exact instructions per output byte, v1.6.10-pre -> now:
level 3 dickens 25.78 -> 15.89, samba 13.43 -> 7.89, xml 7.91 -> 5.08, ooffice 24.42 -> 15.22, mr 14.43 -> 6.27.

**What each step bought** (instructions per byte unless noted; each step is gated: exact-size source and destination under
ASAN/UBSan on both the BMI2 and the plain decoder, 300000 mutants per clone compared with the previous decoder on return code
and an FNV hash of the output, tiny blocks, edge cases, the two memory-safety PoCs):

- R0, the stage-A/B rewrite: dickens -30..-33%, xml -30%, samba -26..-29%, ooffice -18..-28%, mr -7..-13%.
- PM (SSE2 plane merge) + H1 (Huffman lockstep): mr -48..-52%, sao -27..-46%, x-ray -36..-43%, mozilla -13..-24%.
- R1 (negated-position bit reader, bzhi in the BMI2 clone): -1..-8%, -2.2..-2.5% weighted cycles.
- SEQ_AHEAD 16 -> 64: no instruction change; weighted cycles -2.6% (L1), -9.8% (L3), -4.7% (L8), -4.9% (L15), webster -17%.
  The per-file effect is large for the files whose matches miss every cache level (webster, mozilla, samba) and within noise for
  the rest; nci and osdb move by +-3..8% between runs of the same binary at 1.2-3 cycles/byte, so no 16-vs-64 difference on
  them is believed below that.
- Not kept: unconditional 2x16 B match copy (R3: -0.8% weighted at L3 but dickens +1.8%, sao +2.1% elsewhere; with a per-block
  picker the duplicated code lost 1.5-5%), a runtime-chosen SEQ_AHEAD (+2.5-4% instructions), the rep history in two registers
  (0.6 instructions per sequence), keeping the "explicit offset == rep0 leaves the history alone" exception (+2-4%
  instructions; the encoder never emits such a code, so the rule was made uniform and pinned by bakeoff/gate/rpzero.c).

Measurement notes that cost time: a stopped workflow does not stop the background jobs its agents started; cycles of 1-3
cycles/byte files swing +-10% when the box is loaded while instruction counts do not move, so decisions were taken on
instructions first and on interleaved best-of-12 cycles on an otherwise idle core second.
