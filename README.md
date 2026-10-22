# lz6

**lz6** is an experimental lossless compressor: maximum compression with fast decompression. It is meant as the successor of LZ4 and LZ5 v1.5, and it competes with [zstd], [lizard] and [misa77].

It started as a fork of [LZ5 v1.5] (itself a LZ4 derivative) and grew into its own codec family:

- **Frame LZ/HC codec** — the classic LZ5-style byte-wise matcher, hardened and tuned.
- **seq engine** — an LZ match finder feeding an FSE/rANS entropy stage, with per-data-type transforms (byte-plane for interleaved floats, PRNG regeneration for pseudo-random data, order-0/order-1/huffman literal coding).
- **LZ6S2** — per-block codec dispatch: every block inside one frame picks its own engine (seq / light-LZ / raw escape).
- **LZ6S3** — content segmentation: the compressor tracks the byte-entropy profile while reading and cuts blocks at content shifts, so each region gets its own literal mode and transform.
- **x86 branch filter** — CALL/JMP `rel32` operands are converted to absolute addresses on detected code ranges, only where a trial shows it pays (Silesia `ooffice`, a Windows DLL: 44.4% -> 38.2% at level 15).
- **Multithreaded command line** — `-T#` runs blocks on a worker pool for compression (the seq levels and `--fast`) and decompression (any frame). The output is identical at any thread count (see [Multithreading](#multithreading)).

Current release: **v1.6.13-pre** ([releases](https://github.com/YadeWira/lz6/releases): static Linux x86-64, win64 and win32 binaries).

New in v1.6.13-pre (see [NEWS](NEWS)): **levels `-2` to `-7` are retuned** (encoder only: no format change and no decoder change, every stream decodes with v1.6.12-pre; the fast tier, `-1` and `-8` to `-13` write the same bytes). Silesia: `-2` 28.09% -> 27.65%, `-3` 27.45% -> 27.35%, `-4` 27.32% -> 27.23%, `-5` 27.21% -> 26.98%, `-6` 26.90% -> 26.44%, `-7` 26.60% -> 26.08%, with the decode cycles 0.4-1.5% lower at every one of them and the encode 1-24% dearer. The [Benchmarks](#benchmarks) section starts with the comparison.

Earlier, v1.6.12-pre (see [NEWS](NEWS)): **the seq level numbers move** (`-1`..`-13`, plus a `--fast[=N]` tier: v1.6.11-pre's `-N` is `-(N-2)` for N >= 3, same bytes), a polished fast tier (Silesia 33.09% -> 32.56% and 31.58% -> 30.45%), a **level guard** that stops levels 3-15 from losing to level 1 on small-alphabet data (AIT A -4%, 2x faster to decode), and **byte-delta plane lanes** (`mr` -5.7%, `x-ray` -2.2% at level 13, both decode faster; a seq format extension: streams that use them need v1.6.12-pre, every older stream still decodes). Silesia level 13: 25.12% -> 24.98%. The [Benchmarks](#benchmarks) section starts with the v1.6.12-pre against v1.6.11-pre comparison.

Earlier, v1.6.11-pre: a rewritten seq decoder that decodes 1.3-1.7x faster with the same compressed bytes, tighter length-code widths (-0.2..-0.3% bytes, a seq format change: streams written by v1.6.10-pre and earlier do not decode) and two memory-safety fixes for corrupt input.

## Usage

```
lz6 -1 file                    # compress (seq engine: about the ratio of the LZMA family's first level, several times faster)
lz6 -13 file                   # maximum compression
lz6 --fast file                # the fast tier: speed over ratio (--fast=2 is faster still)
lz6 --hc -9 file         # classic LZ6-HC frame codec
lz6 -d file.lz6          # decompress (codec-agnostic, per-block dispatch)
lz6 -1 -c file | lz6 -d -c     # pipes
lz6 -m file1 file2       # multiple inputs
lz6 -1 -B5 -T0 file      # all cores: same bytes as one thread, several times faster
lz6 -d -T4 file.lz6      # decompress on 4 threads (any file, any -T)
```

Levels 1-13 and `--fast[=N]` (the fast tier) select the seq engine (14 and 15 are 13 for now); the default level uses the classic fast LZ frame codec. v1.6.12-pre renumbered the seq levels: v1.6.11-pre's `-N` is `-(N-2)` now for N >= 3, and its `-2` and `-1` are `--fast=1` and `--fast=2` (a smaller, slower fast tier, see [NEWS](NEWS)). Blocks up to 256MB, match windows up to 32MB. Frames are self-describing and independently decodable per block. `lz6 --help` lists every option; the man page is `programs/lz6.1`.

## Multithreading

`-T#` sets the worker threads (`0` = one per core, default `1`). Seq blocks are self-contained, so the command line compresses and decodes them on an ordered pool and writes them back in input order: **the compressed file is byte-for-byte what one thread writes**, and any `.lz6` file decodes with any `-T`. There is no format change.

Silesia.tar (212 MB, one file), `-B5`, wall time, best of 3, one thread on CPU 0 against 16 threads on CPUs 0-15 (distinct cores of one socket) of a shared machine, v1.6.11-pre:

| level | 1 thread | 16 threads | speed-up | decode, 1 -> 16 threads |
|---|---:|---:|---:|---:|
| `-7` | 31.8 s | 5.8 s | 5.5x | 621 -> 1,180 MB/s |
| `-3` | 6.5 s | 1.75 s | 3.7x | 675 -> 1,222 MB/s |
| `-1` | 4.15 s | 1.32 s | 3.1x | 680 -> 1,260 MB/s |
| `--fast=2` | 1.82 s | 0.74 s | 2.5x | 638 -> 1,230 MB/s |

The decode column is whole-process wall time for 212 MB written to `/dev/null` (reading the file, page faults on the output, the content checksum), so it is below the in-process library numbers of the [Benchmarks](#benchmarks) section. With v1.6.10-pre in the same session it is 449 -> 1,126 (`-9`), 473 -> 1,094 (`-5`), 481 -> 1,061 (`-3`) and 454 -> 1,050 MB/s (`-1`): the faster decoder gives +38-49% on one thread and +5-20% on 16, where the 0.17 s wall time is mostly fixed costs and the other tenants of the machine.

Each thread works on one block, so the speed-up is bounded by the number of blocks and by the biggest one. On large inputs the seq levels default to `-B7` (blocks up to 256MB, chosen for ratio), which makes few, large blocks: on 8 threads the same file gains only 2.4-2.6x. `-B5` or `-B4` make more of them and cost 0.6-0.8 points of ratio (`-1`: 29.00% default, 29.56% `-B5`, 29.82% `-B4`) for 3-5.6x on 8 threads; `-B4` is also faster on a single thread. `--fast=2` is limited by the main thread (~270 MB/s).

- Compression is threaded for the seq levels (`-1` to `-13`, `--fast`) only; `--hc` and `-0` compress on one thread (their output depends on the context that compresses a block). Decompression is threaded for every codec.
- Memory grows with `-B` and `-T` (about two blocks per thread, plus the compressor's tables).
- For callers with their own threads, `lz6frame.h` has a block-level API (`LZ6F_compressBlockIndependent()`, `LZ6F_decompressBlockIndependent()`, ...); after `LZ6_seq_init()` the library has no mutable global state.

More numbers (and the other thread counts) are in the wiki's [Benchmarks](https://github.com/YadeWira/lz6/wiki/Benchmarks) page.

## Build

Linux/macOS (gcc or clang, C99):

```
make -C programs lz6
```

The Makefile links pthreads for `-T#`; the Windows build uses Win32 threads and needs no extra DLL.

Windows (mingw-w64 cross-compile from Linux, or MSVC):

```
./build_windows.sh      # win64 + win32, static, Win7 SP1+
```

## Benchmarks

Measured with one harness ([lzbench], one thread, mem-to-mem, per file) on 2x Xeon E5-2697A v4, one core pinned with the machine otherwise idle (load average below 3), on 2026-10-09. lz6 is v1.6.11-pre (the lzbench plugin still labels it 1.6.9-pre) and the competitors (zstd 1.5.7, lizard 2.1, lz5 v1.5, misa77 0.6.0, lz4 1.10.0, memlz 0.2 beta) were run in the same session; the uf-lzma2 row is from the 2026-10-01 session. Speeds of different sessions are not comparable (an idle core boosts higher), so no row here is mixed with older runs. These are single-thread library numbers, not the [Multithreading](#multithreading) ones. The full matrix of the previous release (all levels, plus brotli, xz, zlib and fastlzma2) is in [bakeoff/VS_CODECS.md](bakeoff/VS_CODECS.md), which also has the per-level rows of this tree:

### v1.6.13-pre against v1.6.12-pre

Levels `-2` to `-7` only (the other levels write the same bytes in both versions). One pinned core, the 19 files of Silesia and AIT A, B, C, E, F, G, H (AIT D left out); sizes are exact, the decode and encode costs are exact user-mode cycle counts (perf counters around the decode loop and around the compress call, interleaved rounds, size-weighted), because the wall-clock speed of lzbench on this shared machine moves by 10-20% between sessions.

| level | Silesia size | AIT size | decode cycles | encode cycles |
|---|---:|---:|---:|---:|
| `-2` | 28.09% -> **27.65%** | 46.32% -> 46.25% | -0.4% | +14% |
| `-3` | 27.45% -> **27.35%** | 46.21% -> 46.20% | -0.6% | +6% |
| `-4` | 27.32% -> **27.23%** | 46.20% -> 46.18% | -0.5% | +1% |
| `-5` | 27.21% -> **26.98%** | 46.13% -> 46.10% | -0.4% | +24% |
| `-6` | 26.90% -> **26.44%** | 45.86% -> 45.80% | -1.5% | +12% |
| `-7` | 26.60% -> **26.08%** | 45.66% -> 45.58% | -0.8% | +19% |

What changed: the row-hash parser of `-2` and `-4` probes rep1 and rep2 (a rep1/rep2 match is charged 16 bits, because the decoder's offset history takes more cycles on them), `-2` searches its 16 slots (was 8), `-3` 32 candidates (was 24), `-5` follows 16 chain candidates (was 8), and the binary tree of `-6` and `-7` searches 10 candidates (was 8). **Larger than before** (all within 0.11%): `-3` `sao` +0.014%; `-4` `x-ray` +0.104%, `sao` +0.012%; `-5` `sao` +0.063%, `mr` +0.008%. Files that decode more than 3% slower than at the same level before: `-2` `sao` +5.4%, `-3` `sao` +4.2%, `-4` `ait_H` +3.1%, `-7` `nci` +3.9%. Per file and level: [bakeoff/V1613_VS_V1612.md](bakeoff/V1613_VS_V1612.md).

Against the LZMA family (measured 2026-10-22, one thread, lzbench in memory; sizes are Silesia): xz -2/-3/-4/-5 give 26.88/26.38/24.70/23.67% at 12/8.6/5.1/3.7 MB/s, fast-lzma2 and uf-lzma2 -2/-3/-4/-5 26.59/25.46/24.57/24.12% at 17/11/9.3/7.9 MB/s, lzma (SDK) -2/-3/-4/-5 26.93/26.53/26.39/23.41% at 21/20/15/3.4 MB/s. Read against lz6's own curve at the same encode speed (`-4` 27.23% at about 26 MB/s, `-5` 26.98% at about 12, `-6` 26.44% at about 7, `-7` 26.08% at about 5): xz -2 and lzma -2 are level with lz6, lzma -3/-4 and xz -3 are 1-2% smaller, xz -4/-5, fast-lzma2 and uf-lzma2 -3 to -5 are 5-9% smaller. They decode at 80-120 MB/s; lz6 decodes at about 1,000.

### v1.6.12-pre against v1.6.11-pre

Same harness and machine, one pinned idle core (CPU 44), both versions built into one lzbench each and run interleaved (A/B/B/A, best of two passes), 2026-10-19; the 19 files of Silesia and AIT A, B, C, E, F, G, H (AIT D, the regenerated `random()` file, is left out of this table); size-weighted over each corpus. Rows are the same engine rows: v1.6.12-pre's `-N` is v1.6.11-pre's `-(N+2)`, and its `--fast=2` and `--fast=1` are v1.6.11-pre's `-1` and `-2`. The lzbench plugin labels both builds with a stale version string.

| row (v1.6.11-pre level) | Silesia size | enc MB/s | dec MB/s | AIT size | enc MB/s | dec MB/s |
|---|---:|---:|---:|---:|---:|---:|
| `--fast=2` (1) | 33.09% -> **32.56%** | 168 -> 133 | 1,029 -> 1,073 | 47.90% -> **47.70%** | 197 -> 159 | 1,107 -> 1,085 |
| `--fast=1` (2) | 31.58% -> **30.45%** | 142 -> 109 | 950 -> 986 | 47.52% -> **47.10%** | 167 -> 123 | 1,106 -> 1,077 |
| `-1` (3) | 28.79% -> **28.77%** | 63.4 -> 58.9 | 1,072 -> 1,070 | 46.70% -> 46.70% | 89 -> 83 | 1,109 -> 1,109 |
| `-3` (5) | 27.54% -> **27.45%** | 31.9 -> 29.3 | 1,106 -> 1,114 | 46.46% -> **46.21%** | 48.5 -> 36.7 | 1,022 -> 1,091 |
| `-6` (8) | 26.99% -> **26.90%** | 8.1 -> 7.7 | 1,010 -> 1,030 | 46.15% -> **45.86%** | 9.9 -> 9.3 | 947 -> 1,046 |
| `-10` (12) | 25.22% -> **25.09%** | 2.8 -> 2.6 | 1,096 -> 1,128 | 45.77% -> **45.48%** | 5.6 -> 5.3 | 1,004 -> 1,082 |
| `-13` (15) | 25.12% -> **24.98%** | 0.7 -> 0.7 | 1,109 -> 1,099 | 45.64% -> **45.36%** | 1.5 -> 1.5 | 989 -> 1,075 |

Sizes are exact and do not depend on the session. The decode speeds of single files move by up to +-6% between passes of the same binary on identical bytes (mozilla at `-13`: -6.3%, with the same compressed bytes), so read the table as "no change" where the difference is a few percent: the exact decode cycles (perf counters, CPU 44, best of 3 interleaved rounds, size-weighted) are the reliable number. Silesia, v1.6.11-pre -> v1.6.12-pre: `--fast=2` -3.5%, `--fast=1` -3.4%, `-1` 0.0%, `-3` -0.2%, `-6` -0.2%, `-10` -0.7%; AIT: -0.6%, -0.4%, -3.3%, -9.4%, -10.8%, -10.1%; per file, `mr` -4% to -6% at every level from 2, `x-ray` -12% at `-10` and -21% at `-13`, AIT A -22% to -54%. A file whose compressed bytes did not change decodes with the same instructions as before (five such files that showed a one-off +2% to +18% in the first pass were measured again, five rounds each: -0.3% to +0.1%). Files that decode slower after a change of their parse: AIT A +2.4% and +1.0% at `--fast=2` and `--fast=1`, AIT H +1.6% (`--fast=1`), nci +1.3% (`--fast=1`), AIT F +2.1% at `-13` and +0.8% at `-3`.

The price: encode is 21-24% slower on the two `--fast` rows (the polish of the fast parser), 7-8% slower at `-1` and `-3` (AIT at `-3`: 24%, the level guard's second parse on AIT A), 5-7% at `-6` and `-10`, and the same at `-13`.

**Larger than before**, in bytes: AIT F +24 B (+0.001%) at every row; AIT G +98 B (+0.013%) at `-1`..`-13`, +54 B (+0.007%) at `--fast=2` and +507 B (+0.069%) at `--fast=1`; AIT A +153 B and +151 B (+0.022%) at `--fast=2` and `-1`. Every other file of the 19 is the same size or smaller at every row (`ait_A` -4.0% to -5.4% at `-2`..`-13`, `mr` -5.7% from `-2`, `x-ray` up to -2.2%, 14 of 19 files byte-identical from `-1` up). Per file and row: [bakeoff/V1612_VS_V1611.md](bakeoff/V1612_VS_V1611.md).

### Against the LZMA family and zpaq

Same harness and machine, one thread on CPU 44, the 19 files, 2026-10-19; lz6 as of the v1.6.12-pre renumbering but **before** the byte-delta lanes, so its speeds are from that session while its Silesia size is the current one (24.98%; it was 25.12% in the session; the decode speed of `-13` did not move beyond noise, 1,109 -> 1,099 in the comparison above); uf-lzma2 v1.6.3 sources, xz (liblzma LZMA2, `xz -9`), LZMA SDK 26.01 (`lzma -9`), fast-lzma2, zpaq 7.15 and zpaqfranz through their command lines (`-m5 -t1`, whole process, tmpfs, one round), RAR 7.23 (`-m5 -mt1`, whole process, three rounds, best per file; closed source, command line only).

| codec | Silesia | dec MB/s | AIT (7 files, no D) | dec MB/s |
|---|---:|---:|---:|---:|
| **lz6 -13** | **24.98%** | **1,118** | **45.36%** | **1,096** |
| xz -9 (LZMA2) | 23.02% | 115 | 45.92% | 57 |
| lzma -9 | 23.00% | 88 | 46.00% | 44 |
| fast-lzma2 -10 | 22.97% | 90 | 45.50% | 40 |
| uf-lzma2 -10 | 22.97% | 124 | 45.50% | 55 |
| uf-lzma2 -11 | 22.88% | 123 | 44.13% | 55 |
| RAR -m5 | 25.07% | 159 (process) | 48.62% | 71 (process) |
| zpaq -m5 / zpaqfranz -m5 | 18.45% | 0.35 | 40.79% | 0.36 |

lz6 `-13` decodes 9-13x faster than the LZMA family on Silesia (19-27x on AIT), at a size about 2 points (8-9%) larger: 24.98% against 23.02% for xz -9 and 22.88% for uf-lzma2 -11. The gaps against xz -9 per file: sao +15%, mozilla +15%, samba +12%, mr +12% (it was +19% before the byte-delta lanes), xml +10%; lz6 is ahead on `ooffice`, AIT E and F, and on AIT as a whole against xz, lzma and RAR. RAR -m5 (25.07% on Silesia, 48.62% on AIT) is level with lz6 `-13` on Silesia and 3 points behind on AIT; its decode figure is whole-process wall clock, not comparable with the in-memory ones. zpaq -m5 is 19-26% smaller than the rest and about 3,000x slower to decode. Entry level: lz6 `-1` (28.77%) against `xz -1` (27.70%, 17 MB/s encode, 97 decode) and `lzma -1` (27.58%, 22/74): 4% bigger, 3x the encode speed and 11-14x the decode speed. Per file and the raw tables: [bakeoff/VS_LZMA_ZPAQ.md](bakeoff/VS_LZMA_ZPAQ.md).

**Level labels.** The numbers below were measured with v1.6.11-pre; the labels are v1.6.12-pre's (renumbered: v1.6.11-pre's `-N` is `-(N-2)` for N >= 3, and its `-2` and `-1` are `--fast=1` and `--fast=2`). The two `--fast` rows below are v1.6.11-pre's fast levels (its `-1` and `-2`): v1.6.12-pre's are 0.5 and 1.1 points smaller (`--fast=2` 32.56%, `--fast=1` 30.45% on Silesia) and encode 21-24% slower, see the comparison above and [NEWS](NEWS).

**Silesia** (212 MB, 12 files)

| codec | ratio | enc MB/s | dec MB/s |
|---|---:|---:|---:|
| uf-lzma2 -11 (asm decoder) | 22.88% | 1.2 | 118 |
| zstd -19 | 24.96% | 2.8 | 807 |
| **lz6 -13** | **25.12%** | 0.7 | **1,129** |
| **lz6 -10** | **25.22%** | 2.9 | **1,144** |
| zstd -17 | 25.58% | 5.1 | 837 |
| **lz6 -6** | **26.99%** | 8.4 | **1,051** |
| zstd -14 | 27.03% | 11.8 | 881 |
| zstd -12 | 27.33% | 27.8 | 862 |
| zstd -9 | 27.88% | 54.1 | 857 |
| lizard -49 | 28.62% | 1.9 | 1,209 |
| **lz6 -1** | **28.79%** | 65.1 | **1,092** |
| zstd -6 | 28.89% | 75.8 | 890 |
| zstd -5 | 29.61% | 102 | 828 |
| lz5 v1.5 HC -15 | 30.95% | 2.2 | 793 |
| zstd -3 | 31.21% | 171 | 874 |
| **lz6 --fast=1** | **31.58%** | 144 | **967** |
| **lz6 --fast=2** | **33.09%** | 170 | **1,043** |
| zstd -1 | 34.55% | 360 | 1,207 |
| lizard -30 | 40.47% | 328 | 1,235 |
| misa77 -1 | 42.65% | 52.0 | 5,205 |
| lz4 | 47.60% | 555 | 3,663 |
| memlz | 59.91% | 1,129 | 1,096 |

**AIT A-H** (13 MB, 8 files)

| codec | ratio | enc MB/s | dec MB/s |
|---|---:|---:|---:|
| **lz6 -13** | **38.69%** | 1.9 | **834** |
| **lz6 -6** | **39.12%** | 11.3 | **811** |
| **lz6 -1** | **39.59%** | 101 | **898** |
| **lz6 --fast=1** | **40.29%** | 183 | **896** |
| **lz6 --fast=2** | **40.61%** | 216 | **898** |
| uf-lzma2 -11 (asm decoder) | 52.64% | 2.2 | 64.8 |
| zstd -19 | 55.97% | 5.7 | 1,097 |
| lizard -49 | 61.21% | 5.8 | 1,560 |
| lz5 v1.5 HC -15 | 65.49% | 2.1 | 1,014 |
| misa77 -1 | 73.29% | 52.7 | 7,956 |
| lz4 | 75.66% | 791 | 5,156 |
| memlz | 81.64% | 1,910 | 1,713 |

On AIT most of lz6's lead comes from one file, D (glibc `random()` output, regenerated from its seed: 2 MB -> 7 bytes); without D, lz6 -13 is 45.64%, ahead of xz -9 and zstd -19 (48.06%) and behind brotli -11 and LZMA2.

**On Silesia, on one thread, lz6 now decodes faster than the zstd ladder at the same ratio from `--fast=1` up, and still encodes slower.** Against zstd of the same session: lz6 --fast=1 (31.58%) decodes 1.11x faster than zstd -3 (31.21%) and encodes at 0.84x its speed; lz6 -1 (28.79%) is smaller than zstd -6 (28.89%), decodes 1.23x faster and encodes at 0.86x; lz6 -6 (26.99%) matches zstd -14 (27.03%), decodes 1.19x faster and encodes at 0.71x; lz6 -10 (25.22%) is smaller than zstd -17 (25.58%), decodes 1.37x faster and encodes at 0.57x; lz6 -13 (25.12%) is 0.16 points behind zstd -19 (24.96%), decodes 1.40x faster and encodes 3.8x slower. At the fast end zstd -1 (34.55%) still decodes 1.16x faster than lz6 --fast=2 (33.09%, 1.46 points smaller), and lizard -30, misa77 and lz4 decode faster still at much worse ratios. lizard -49 (28.62%) decodes 1.11x faster than lz6 -1 (28.79%) but encodes at 1.9 MB/s; lz6 -3 (27.54%, 33 MB/s encode, 1,144 MB/s decode) is 1.1 points smaller and decodes at 0.95x its speed. lz6 -13 is still ahead of lizard, lz5 and misa77 in ratio. What moved: v1.6.10-pre decoded at 633-684 MB/s on this machine in the same session (levels 1, 3, 8 and 15), 1.65-1.7x slower; its decoder ran about 250 instructions per sequence against 121-136 for zstd's, and now runs 143-163 (xml, dickens, samba at level 3) at about the same IPC (see [NEWS](NEWS)).

Where lz6 leads is in its transforms. On `ooffice` (x86 code) lz6 -13 reaches 38.15% against 42.18% for zstd -19, and lz6 -3 41.23% against 46.00% for zstd -12; on `x-ray` (byte planes) lz6 -3 gets 55.80% against 62.68%; on `sao` it is 0.5 points ahead, and on the other files it is 0.2-1.7 points behind zstd -12 at a similar encode speed. And blocks are independent, so decoding scales with `-T` (see [Multithreading](#multithreading)). See also [bakeoff/COMPETITORS.md](bakeoff/COMPETITORS.md) and the [wiki](https://github.com/YadeWira/lz6/wiki/Benchmarks).

## Documentation

- [lz6_Block_format.md](lz6_Block_format.md) — raw block format
- [lz6_Frame_format.md](lz6_Frame_format.md) — frame format (block codec flags, per-block dispatch)
- [bakeoff/COMPETITORS.md](bakeoff/COMPETITORS.md) — competitive analysis vs zstd, lizard, misa77
- [bakeoff/VS_LZMA_ZPAQ.md](bakeoff/VS_LZMA_ZPAQ.md) — lz6 against xz, lzma, fast-lzma2, uf-lzma2, zpaq, zpaqfranz and RAR (first level and top levels, per file)
- [bakeoff/V1612_VS_V1611.md](bakeoff/V1612_VS_V1611.md) — v1.6.12-pre against v1.6.11-pre, every row and file (size, speed, exact decode cycles)
- [NEWS](NEWS) — what changed in each release, including measured costs and refuted ideas
- `lib/entropy/lz6seq.h` — the seq codec API
- [Wiki](https://github.com/YadeWira/lz6/wiki) — architecture, seq stream format, decoder internals, development workflow, roadmap

## Status

Experimental: the seq block format can change between `-pre` versions without a compatibility shim (see [NEWS](NEWS)); v1.6.10-pre did not change it, v1.6.11-pre does (tighter length-code widths: streams written by earlier builds do not decode), and v1.6.12-pre extends it (byte-delta plane lanes: every v1.6.11-pre stream still decodes, but a stream that uses them needs v1.6.12-pre). v1.6.13-pre changes only the encoder. The frame HC levels (`--hc`) are stable. The seq codec and the frame layer are fuzz-tested (corrupt raw streams + corrupted frames through the full `LZ6F_decompress` path), and the multithreaded paths were run under AddressSanitizer, UBSan and ThreadSanitizer. Windows binaries (win64 and win32) are VM-tested on Windows 7 SP1 and Windows 10.

[LZ5 v1.5]: https://github.com/inikep/lz5
[zstd]: https://github.com/facebook/zstd
[lizard]: https://github.com/inikep/lizard
[misa77]: https://github.com/welcome-to-the-sunny-side/misa77
[lzbench]: https://github.com/inikep/lzbench
