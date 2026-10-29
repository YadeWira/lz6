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

New in v1.7.0, the **first stable release** (see [NEWS](NEWS)): the formats are frozen (a stream written by v1.6.11-pre or later decodes with every later version; the seq stream is now specified in [lz6_Seq_format.md](lz6_Seq_format.md)) and a test campaign before it found and fixed four bugs that the `-pre` series had. **Compressor:** a block with a run of more than 16 MiB of literals or a match, or more than 2^24 sequences, gave a stream that did not decode, and `lz6 -1 file` was enough to hit it on a file larger than 16 MiB (40 MB of zeros, compressed ISOs and disk images); the compressed bytes at the chain and tree levels and with `--hc` depended on the address of the source buffer (a second, equally valid stream in about 1% of the runs of the static Linux binary, every run without ASLR). **Decoder:** the plain block decoder read one byte past a 1-byte input. **Command line:** an unanswered overwrite prompt (stdin closed) overwrote the file, a failed read was taken for the end of the file (hang, or a shorter frame reported as success), `-t` accepted any file, `-B8` meant `-8`. Plus the library's `make install` (the `.a` held source files, the `.so` was not linked with libm, the pkg-config file named lz4). For ordinary inputs the compressed bytes are the same as v1.6.15-pre's, and the decode costs 0.5-3.2% fewer cycles than v1.6.11-pre's at the seven levels measured (see [NEWS](NEWS)).

Earlier releases (v1.6.11-pre to v1.6.15-pre: a faster seq decoder, the renumbered levels `-1`..`-13` and `--fast[=N]`, the level guard, byte-delta lanes, every level retuned) are described in [NEWS](NEWS), with the per-file comparisons in `bakeoff/V16*_VS_*.md`.

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
- [bakeoff/VS_LZMA_ZPAQ.md](bakeoff/VS_LZMA_ZPAQ.md) — lz6 against xz, lzma, fast-lzma2, uf-lzma2, zpaq, zpaqfranz and RAR (first level and top levels, per file)
- [bakeoff/V1612_VS_V1611.md](bakeoff/V1612_VS_V1611.md) — v1.6.12-pre against v1.6.11-pre, every row and file (size, speed, exact decode cycles)
- [bakeoff/V1613_VS_V1612.md](bakeoff/V1613_VS_V1612.md) — v1.6.13-pre against v1.6.12-pre, levels -2 to -7, every file (size, exact decode and encode cycles)
- [bakeoff/V1614_VS_V1613.md](bakeoff/V1614_VS_V1613.md) — v1.6.14-pre against v1.6.13-pre, levels -9 to -13, every file (size, exact decode and encode cycles)
- [bakeoff/V1615_VS_V1614.md](bakeoff/V1615_VS_V1614.md) — v1.6.15-pre against v1.6.14-pre, levels -1 and -8, every file (size, exact decode and encode cycles)
- [bakeoff/fuzz/README.md](bakeoff/fuzz/README.md), `bakeoff/seqgate.sh`, `bakeoff/clitest.sh`, `bakeoff/installtest.sh` — the libFuzzer harnesses, the safety gate, the command-line tests and the install test
- [NEWS](NEWS) — what changed in each release, including measured costs and refuted ideas
- `lib/entropy/lz6seq.h` — the seq codec API
- [Wiki](https://github.com/YadeWira/lz6/wiki) — architecture, seq stream format, decoder internals, development workflow, roadmap

## License

lz6 started as a fork of LZ5 v1.5, itself a LZ4 derivative, so the tree carries three licenses, by origin:

| Part | License |
|---|---|
| lz6's own code: `lib/entropy/` (the seq engine), `programs/lz6mt.h`, `bakeoff/`, `.github/`, the format documents | [MIT](LICENSE), Copyright (c) 2026 Yade Bravo |
| `lib/` derived from LZ4/LZ5: `lz6.c`, `lz6hc.c`, `lz6frame.c`, `xxhash.c` | BSD 2-Clause, Copyright (c) Yann Collet ([lib/LICENSE](lib/LICENSE)) |
| `programs/` derived from the LZ4/LZ5 command line: `lz6cli.c`, `lz6io.c`, `bench.c`, `datagen*` | GPL v2 or later ([programs/COPYING](programs/COPYING)) |

The library is MIT and BSD (permissive). The `lz6` command-line program links the GPL-derived files, so the binary is GPL v2 or later.

## Status

**Stable since v1.7.0.** The compressed formats (the frame format, the plain block format and the seq stream) are frozen: a stream written by v1.6.11-pre or any later version decodes with every later version, and the decoder keeps accepting everything the format allows. Streams written before v1.6.11-pre do not decode (that was the last format change without a shim). The shared library's soname is `liblz6.so.1`; the documented API is what `lz6.h`, `lz6hc.h`, `lz6frame.h` and `lz6seq.h` declare (the shared library also exports internal helpers, which are not part of it).

What is **not** promised: the compressed bytes an encoder writes for the same input. Levels are retuned between versions, and 32-bit and 64-bit builds write different bytes for some inputs (every stream decodes everywhere; aarch64 and x86-64 write the same bytes). Within one build the output does not depend on the thread count or on where the input sits in memory.

How it is tested: the safety gate (`bakeoff/seqgate.sh`: ASAN and UBSan on the decoders, the encoder round trips, mutation fuzzing of both decoder variants against the previous release, the 32-bit build), five libFuzzer harnesses (`bakeoff/fuzz`), MemorySanitizer and ThreadSanitizer runs of the command line, 77 command-line checks (`bakeoff/clitest.sh`, including injected read errors), the streams of v1.6.11 to v1.6.15 (decoded three ways), aarch64 and armhf under qemu, i386 with and without SSE2, a round trip of a 39,778-file corpus of format samples, and Windows 7 SP1 and Windows 10 (win64 and win32, real VMs). Known limits: only little-endian hosts are tested; the "incompressible" shortcut of the seq encoder looks at the first 32 KB of a block; a library call has a setup cost of ~15 ms at levels 3 and up (it matters for tiny inputs).

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
