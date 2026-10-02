# Single thread: where lz6 stands against zstd, and why

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
