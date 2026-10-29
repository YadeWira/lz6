# lz6

**lz6** is a lossless compressor: the ratio class of the LZMA family with a decoder an order of magnitude faster. On Silesia, `-13` is within 8% of xz -9 and decodes about 10x faster; on the AIT set it is smaller than xz, lzma and fast-lzma2. It competes with xz / lzma, [fast-lzma2], [uf-lzma2], zpaq and RAR in ratio, and with [zstd], [lizard] and [misa77] in decode speed (see [Benchmarks](#benchmarks)). It is the successor of LZ4 and LZ5 v1.5, and since v1.7.0 its formats are stable (see [Status](#status)).

| Component | Status |
|---|---|
| CI | [![CI][ciBadge]][ciLink] |
| Latest release | [![Release][relBadge]][relLink] |

It started as a fork of [LZ5 v1.5] (itself a LZ4 derivative) and grew into its own codec family:

- **Frame LZ/HC codec** — the classic LZ5-style byte-wise matcher, hardened and tuned.
- **seq engine** — an LZ match finder feeding an FSE/rANS entropy stage, with per-data-type transforms (byte-plane for interleaved floats, PRNG regeneration for pseudo-random data, order-0/order-1/huffman literal coding).
- **LZ6S2** — per-block codec dispatch: every block inside one frame picks its own engine (seq / light-LZ / raw escape).
- **LZ6S3** — content segmentation: the compressor tracks the byte-entropy profile while reading and cuts blocks at content shifts, so each region gets its own literal mode and transform.
- **x86 branch filter** — CALL/JMP `rel32` operands are converted to absolute addresses on detected code ranges, only where a trial shows it pays (Silesia `ooffice`, a Windows DLL: 44.4% -> 38.2% at level 15).
- **Multithreaded command line** — `-T#` runs blocks on a worker pool for compression (the seq levels and `--fast`) and decompression (any frame). The output is identical at any thread count (see [Multithreading](#multithreading)).

Current release: **v1.7.0** ([releases](https://github.com/YadeWira/lz6/releases): static Linux x86-64, win64 and win32 binaries).

New in v1.7.0, the **first stable release**: the formats are frozen (the seq stream is specified in [lz6_Seq_format.md](lz6_Seq_format.md)), and the test campaign before it fixed four bugs of the `-pre` series (blocks over the format limits that did not decode, an encoder output that depended on the address of the input, a 1-byte over-read of the block decoder, several command-line errors) and the library's `make install`. See [NEWS](NEWS); the history of the `-pre` releases is in the [wiki](https://github.com/YadeWira/lz6/wiki/Home).

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

The decode column is whole-process wall time (read, page faults, checksum), so it is below the in-process library numbers of [Benchmarks](#benchmarks).

Each thread works on one block, so the speed-up is bounded by the number of blocks: on large inputs the seq levels default to `-B7` (blocks up to 256MB, chosen for ratio), and `-B5` or `-B4` make more blocks for 0.6-0.8 points of ratio and a better speed-up (details in the [wiki](https://github.com/YadeWira/lz6/wiki/Benchmarks)).

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

One thread, memory to memory, per file, on 2x Xeon E5-2697A v4 with one core pinned and the machine otherwise idle (the [lzbench] harness, 2026-10-19; zpaq, zpaqfranz and RAR through their command lines, whole process). The 12 files of Silesia (212 MB) and the 7 files of AIT A, B, C, E, F, G, H (11 MB; file D, glibc `random()` output that lz6 regenerates from its seed, is left out). The competitors are uf-lzma2 v1.6.3, xz (liblzma, `xz -9`), LZMA SDK 26.01 (`lzma -9`), fast-lzma2, zpaq 7.15, zpaqfranz and RAR 7.23 (`-m5`, closed source). The sizes of lz6 are those of v1.7.0 (exact); its decode speed is from the same session (the decoder has not slowed since: see [NEWS](NEWS)).

| codec | Silesia | dec MB/s | AIT (7 files) | dec MB/s |
|---|---:|---:|---:|---:|
| **lz6 -13** | **24.87%** | **1,118** | **45.35%** | **1,096** |
| **lz6 -1** | **28.33%** | **1,070** | **46.64%** | **1,109** |
| xz -9 (LZMA2) | 23.02% | 115 | 45.92% | 57 |
| lzma -9 | 23.00% | 88 | 46.00% | 44 |
| fast-lzma2 -10 | 22.97% | 90 | 45.50% | 40 |
| uf-lzma2 -10 | 22.97% | 124 | 45.50% | 55 |
| uf-lzma2 -11 | 22.88% | 123 | 44.13% | 55 |
| RAR -m5 | 25.07% | 159 (process) | 48.62% | 71 (process) |
| zpaq -m5 / zpaqfranz -m5 | 18.45% | 0.35 | 40.79% | 0.36 |

lz6 `-13` decodes 9-13x faster than the LZMA family on Silesia (19-27x on AIT) at a size about 8% larger (24.87% against 23.02% for xz -9); on AIT it is smaller than xz, lzma, RAR and fast-lzma2 (uf-lzma2 -11 reaches 44.13%). RAR -m5 is level with `-13` on Silesia and 3 points behind on AIT (its decode figure is whole-process wall clock, not comparable with the in-memory ones). zpaq -m5 is 19-26% smaller than the rest and about 3,000x slower to decode. Entry level: `-1` (28.33%) against `xz -1` (27.70%, 17 MB/s encode, 97 decode): 2% bigger, 3x the encode speed and 11x the decode speed.

Through the command line a file is cut into blocks by content (`-B7` for big files), so the size of a big file can differ from the one-block library figure: with this tree `-13 -B7` gives 25.14% on Silesia (mozilla +3%) and 45.35% on AIT.

The encode side is the price: `-13` compresses at about 0.7 MB/s (`-1` at 60 MB/s). The tables against zstd, lizard, misa77, lz4 and lz5 (where lz6 decodes 1.1-1.4x faster than zstd at the same ratio from `--fast=1` up), every level, the per-version comparisons and the per-file figures are in the wiki's [Benchmarks](https://github.com/YadeWira/lz6/wiki/Benchmarks) page, [bakeoff/VS_LZMA_ZPAQ.md](bakeoff/VS_LZMA_ZPAQ.md) and [bakeoff/COMPETITORS.md](bakeoff/COMPETITORS.md). Decoding also scales with `-T` (see [Multithreading](#multithreading)).

## Documentation

- [lz6_Block_format.md](lz6_Block_format.md) — raw block format
- [lz6_Frame_format.md](lz6_Frame_format.md) — frame format (block codec flags, per-block dispatch)
- [lz6_Seq_format.md](lz6_Seq_format.md) — the seq stream: one block of the seq engine, every layout, the limits, and what is frozen
- [bakeoff/COMPETITORS.md](bakeoff/COMPETITORS.md) — competitive analysis vs zstd, lizard, misa77
- [bakeoff/VS_LZMA_ZPAQ.md](bakeoff/VS_LZMA_ZPAQ.md) — lz6 against xz, lzma, fast-lzma2, uf-lzma2, zpaq, zpaqfranz and RAR, per file
- [bakeoff/](bakeoff/) — the per-version comparisons (`V16*_VS_*.md`), the benchmark scripts and the test harnesses
- [bakeoff/fuzz/README.md](bakeoff/fuzz/README.md), `bakeoff/seqgate.sh`, `bakeoff/clitest.sh`, `bakeoff/installtest.sh` — the libFuzzer harnesses, the safety gate, the command-line tests and the install test
- [NEWS](NEWS) — what changed in each release, including measured costs and refuted ideas
- `lib/entropy/lz6seq.h` — the seq codec API
- [Wiki](https://github.com/YadeWira/lz6/wiki) — architecture, seq stream format, decoder internals, development workflow, roadmap

## History

lz6 began as a fork of LZ5 v1.5. At one point the AI agent that was developing it was set to compete against **ozip**, another compressor, to see which would win; lz6 won. As a trophy it took parts of ozip's entropy backend and built the **seq engine** (matches found by lz6's own match finder, coded as sequences with FSE/ANS, Huffman and rANS). The engine turned out so strong in ratio that it was hard to make fast, and lz6 was left half abandoned as a curiosity.

It was taken up again to compete with the fast compressors, but that fight went to a separate project, **syc**, which produced the algorithm **sylz** (v2, v3). So lz6 changed course: it stopped chasing the fast compressors and aims at the heavyweights, the LZMA family, zpaq and RAR, with a decoder that is far faster than theirs (see [Benchmarks](#benchmarks)).

## License

lz6 started as a fork of LZ5 v1.5, itself a LZ4 derivative, so the tree carries three licenses, by origin:

| Part | License |
|---|---|
| lz6's own code: `lib/entropy/` (the seq engine), `programs/lz6mt.h`, `bakeoff/`, `.github/`, the format documents | [MIT](LICENSE), Copyright (c) 2026 Yade Bravo |
| `lib/` derived from LZ4/LZ5: `lz6.c`, `lz6hc.c`, `lz6frame.c`, `xxhash.c` | BSD 2-Clause, Copyright (c) Yann Collet ([lib/LICENSE](lib/LICENSE)) |
| `programs/` derived from the LZ4/LZ5 command line: `lz6cli.c`, `lz6io.c`, `bench.c`, `datagen*` | GPL v2 or later ([programs/COPYING](programs/COPYING)) |

The library is MIT and BSD (permissive). The `lz6` command-line program links the GPL-derived files, so the binary is GPL v2 or later.

## Status

**Stable since v1.7.0.** The compressed formats (frame, plain block, seq stream) are frozen: a stream written by v1.6.11-pre or any later version decodes with every later version. The shared library's soname is `liblz6.so.1`; the API is what `lz6.h`, `lz6hc.h`, `lz6frame.h` and `lz6seq.h` declare.

**Not promised:** the bytes an encoder writes for the same input (levels are retuned between versions; 32-bit and 64-bit builds differ for some inputs; aarch64 and x86-64 agree). Within one build the output does not depend on the thread count or on where the input sits in memory.

Tested with the safety gate (`bakeoff/seqgate.sh`), libFuzzer harnesses, sanitizer runs, command-line and install tests, a round trip of a 39,778-file corpus, and Windows 7/10 VMs; CI runs on Linux x86-64/arm64, macOS and under ASAN/UBSan. What is covered and the known limits are in the [wiki](https://github.com/YadeWira/lz6/wiki/Development).

[LZ5 v1.5]: https://github.com/inikep/lz5
[zstd]: https://github.com/facebook/zstd
[lizard]: https://github.com/inikep/lizard
[misa77]: https://github.com/welcome-to-the-sunny-side/misa77
[lzbench]: https://github.com/inikep/lzbench
[fast-lzma2]: https://github.com/conor42/fast-lzma2
[uf-lzma2]: https://github.com/YadeWira/ultra-fast-lzma2
[relBadge]: https://img.shields.io/github/v/release/YadeWira/lz6 "Latest release"
[relLink]: https://github.com/YadeWira/lz6/releases/latest
[ciBadge]: https://github.com/YadeWira/lz6/actions/workflows/ci.yml/badge.svg "Continuous Integration"
[ciLink]: https://github.com/YadeWira/lz6/actions/workflows/ci.yml
