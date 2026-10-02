# lz6

**lz6** is an experimental lossless compressor: maximum compression with fast decompression. It is meant as the successor of LZ4 and LZ5 v1.5, and it competes with [zstd], [lizard] and [misa77].

It started as a fork of [LZ5 v1.5] (itself a LZ4 derivative) and grew into its own codec family:

- **Frame LZ/HC codec** — the classic LZ5-style byte-wise matcher, hardened and tuned.
- **seq engine** — an LZ match finder feeding an FSE/rANS entropy stage, with per-data-type transforms (byte-plane for interleaved floats, PRNG regeneration for pseudo-random data, order-0/order-1/huffman literal coding).
- **LZ6S2** — per-block codec dispatch: every block inside one frame picks its own engine (seq / light-LZ / raw escape).
- **LZ6S3** — content segmentation: the compressor tracks the byte-entropy profile while reading and cuts blocks at content shifts, so each region gets its own literal mode and transform.
- **x86 branch filter** — CALL/JMP `rel32` operands are converted to absolute addresses on detected code ranges, only where a trial shows it pays (Silesia `ooffice`, a Windows DLL: 44.4% -> 38.2% at level 15).
- **Multithreaded command line** — `-T#` runs blocks on a worker pool for compression (levels 1-15) and decompression (any frame). The output is identical at any thread count (see [Multithreading](#multithreading)).

Current release: **v1.6.10-pre** ([releases](https://github.com/YadeWira/lz6/releases): static Linux x86-64, win64 and win32 binaries).

## Usage

```
lz6 -2 file              # compress (seq engine, best ratio/speed trade)
lz6 -15 file             # maximum compression
lz6 --hc -9 file         # classic LZ6-HC frame codec
lz6 -d file.lz6          # decompress (codec-agnostic, per-block dispatch)
lz6 -2 -c file | lz6 -d -c     # pipes
lz6 -m file1 file2       # multiple inputs
lz6 -3 -B5 -T0 file      # all cores: same bytes as one thread, several times faster
lz6 -d -T4 file.lz6      # decompress on 4 threads (any file, any -T)
```

Levels 1-15 select the seq engine; the default level uses the fast LZ frame codec. Blocks up to 256MB, match windows up to 32MB. Frames are self-describing and independently decodable per block. `lz6 --help` lists every option; the man page is `programs/lz6.1`.

## Multithreading

`-T#` sets the worker threads (`0` = one per core, default `1`). Seq blocks are self-contained, so the command line compresses and decodes them on an ordered pool and writes them back in input order: **the compressed file is byte-for-byte what one thread writes**, and any `.lz6` file decodes with any `-T`. There is no format change.

Silesia.tar (212 MB, one file), `-B5`, wall time, best of 3, threads on distinct cores of one socket:

| level | 1 thread | 16 threads | speed-up | decode, 1 -> 16 threads |
|---|---:|---:|---:|---:|
| `-9` | 31.1 s | 6.2 s | 5.0x | 458 -> 1,469 MB/s |
| `-5` | 6.7 s | 1.7 s | 3.9x | 468 -> 1,578 MB/s |
| `-3` | 4.4 s | 1.2 s | 3.6x | 445 -> 1,507 MB/s |
| `-1` | 1.8 s | 0.8 s | 2.3x | 445 -> 1,129 MB/s |

Each thread works on one block, so the speed-up is bounded by the number of blocks and by the biggest one. On large inputs the seq levels default to `-B7` (blocks up to 256MB, chosen for ratio), which makes few, large blocks: on 8 threads the same file gains only 2.4-2.6x. `-B5` or `-B4` make more of them and cost 0.4-0.6 points of ratio (`-3`: 29.15% default, 29.56% `-B5`, 29.71% `-B4`) for 3-5.6x on 8 threads; `-B4` is also faster on a single thread. Level 1 is limited by the main thread (~270 MB/s).

- Compression is threaded for levels 1-15 only; `--hc` and `-0` compress on one thread (their output depends on the context that compresses a block). Decompression is threaded for every codec.
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

Measured with one harness ([lzbench], one thread, mem-to-mem, per file) on 2x Xeon E5-2697A v4, with lz6 v1.6.9-pre (the engine is unchanged in v1.6.10-pre, which adds `-T#` to the command line; these are single-thread library numbers, not the [Multithreading](#multithreading) ones). The full matrix (all levels, plus brotli, xz, zlib and fastlzma2) is in [bakeoff/VS_CODECS.md](bakeoff/VS_CODECS.md):

**Silesia** (212 MB, 12 files)

| codec | ratio | enc MB/s | dec MB/s |
|---|---:|---:|---:|
| uf-lzma2 -11 (asm decoder) | 22.88% | 1.2 | 118 |
| zstd -19 | 24.96% | 2.6 | 756 |
| **lz6 -15** | **25.20%** | 0.7 | **668** |
| **lz6 -12** | **25.31%** | 2.6 | **666** |
| **lz6 -8** | **27.08%** | 7.6 | **592** |
| lizard -49 | 28.62% | 1.8 | 1,159 |
| **lz6 -3** | **28.97%** | 57.1 | **594** |
| zstd -5 | 29.61% | 101 | 787 |
| lz5 v1.5 HC -15 | 30.95% | 2.0 | 769 |
| **lz6 -2** | **31.65%** | 135 | **536** |
| **lz6 -1** | **33.16%** | 164 | **620** |
| zstd -1 | 34.55% | 348 | 1,166 |
| lizard -30 | 40.47% | 318 | 1,197 |
| misa77 -1 | 42.65% | 51.2 | 4,933 |
| lz4 | 47.60% | 540 | 3,537 |
| memlz | 59.91% | 963 | 888 |

**AIT A-H** (13 MB, 8 files)

| codec | ratio | enc MB/s | dec MB/s |
|---|---:|---:|---:|
| **lz6 -15** | **38.70%** | 1.7 | **572** |
| **lz6 -8** | **39.13%** | 10.6 | **562** |
| **lz6 -3** | **39.73%** | 94.0 | **583** |
| **lz6 -2** | **40.31%** | 182 | **631** |
| **lz6 -1** | **40.62%** | 213 | **668** |
| uf-lzma2 -11 (asm decoder) | 52.64% | 2.2 | 64.8 |
| zstd -19 | 55.97% | 4.6 | 1,049 |
| lizard -49 | 61.21% | 5.3 | 1,518 |
| lz5 v1.5 HC -15 | 65.49% | 2.1 | 969 |
| misa77 -1 | 73.29% | 51.1 | 7,957 |
| lz4 | 75.66% | 774 | 5,148 |
| memlz | 81.64% | 1,872 | 1,713 |

On AIT most of lz6's lead comes from one file, D (glibc `random()` output, regenerated from its seed: 2 MB -> 7 bytes); without D, lz6 -15 is 45.66%, ahead of xz -9 and zstd -19 and behind brotli -11 and LZMA2. On Silesia lz6 -15 is 0.24 points behind zstd -19 and ahead of lizard, lz5 and misa77; lz6 -3 is smaller than zstd -5, and lz6 -15 beats xz -9 and zstd -19 on ooffice (x86 code). Decode speed remains its weak axis: 1.1-2x below zstd and lizard. See also [bakeoff/COMPETITORS.md](bakeoff/COMPETITORS.md) and the [wiki](https://github.com/YadeWira/lz6/wiki/Benchmarks).

## Documentation

- [lz6_Block_format.md](lz6_Block_format.md) — raw block format
- [lz6_Frame_format.md](lz6_Frame_format.md) — frame format (block codec flags, per-block dispatch)
- [bakeoff/COMPETITORS.md](bakeoff/COMPETITORS.md) — competitive analysis vs zstd, lizard, misa77
- [NEWS](NEWS) — what changed in each release, including measured costs and refuted ideas
- `lib/entropy/lz6seq.h` — the seq codec API
- [Wiki](https://github.com/YadeWira/lz6/wiki) — architecture, seq stream format, decoder internals, development workflow, roadmap

## Status

Experimental: the seq block format can change between `-pre` versions without a compatibility shim (see [NEWS](NEWS)); v1.6.10-pre did not change it. The frame HC levels (`--hc`) are stable. The seq codec and the frame layer are fuzz-tested (corrupt raw streams + corrupted frames through the full `LZ6F_decompress` path), and the multithreaded paths were run under AddressSanitizer, UBSan and ThreadSanitizer. Windows binaries (win64 and win32) are VM-tested on Windows 7 SP1 and Windows 10.

[LZ5 v1.5]: https://github.com/inikep/lz5
[zstd]: https://github.com/facebook/zstd
[lizard]: https://github.com/inikep/lizard
[misa77]: https://github.com/welcome-to-the-sunny-side/misa77
[lzbench]: https://github.com/inikep/lzbench
