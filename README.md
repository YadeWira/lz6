# lz6

**lz6** is an experimental lossless compressor: maximum compression with fast decompression. It is meant as the successor of LZ4 and LZ5 v1.5, and it competes with [zstd], [lizard] and [misa77].

It started as a fork of [LZ5 v1.5] (itself a LZ4 derivative) and grew into its own codec family:

- **Frame LZ/HC codec** — the classic LZ5-style byte-wise matcher, hardened and tuned.
- **seq engine** — an LZ match finder feeding an FSE/rANS entropy stage, with per-data-type transforms (byte-plane for interleaved floats, PRNG regeneration for pseudo-random data, order-0/order-1/huffman literal coding).
- **LZ6S2** — per-block codec dispatch: every block inside one frame picks its own engine (seq / light-LZ / raw escape).
- **LZ6S3** — content segmentation: the compressor tracks the byte-entropy profile while reading and cuts blocks at content shifts, so each region gets its own literal mode and transform.
- **x86 branch filter** — CALL/JMP `rel32` operands are converted to absolute addresses on detected code ranges, only where a trial shows it pays (Silesia `ooffice`, a Windows DLL: 44.4% -> 38.2% at level 15).
- **Multithreaded command line** — `-T#` runs blocks on a worker pool for compression (the seq levels and `--fast`) and decompression (any frame). The output is identical at any thread count (see [Multithreading](#multithreading)).

Current release: **v1.6.11-pre** ([releases](https://github.com/YadeWira/lz6/releases): static Linux x86-64, win64 and win32 binaries).

New in v1.6.11-pre (see [NEWS](NEWS)): a rewritten seq decoder that decodes 1.3-1.7x faster with the same compressed bytes (single thread, Silesia and AIT: 1.65-1.7x and 1.3-1.4x), tighter length-code widths that shrink the output by 0.2-0.3% (this one changes the seq block format: streams written by v1.6.10-pre and earlier do not decode), and two memory-safety fixes for corrupt input. The [Benchmarks](#benchmarks) section below measures it.

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

**Level labels.** The numbers below were measured with v1.6.11-pre; the labels are v1.6.12-pre's (renumbered: v1.6.11-pre's `-N` is `-(N-2)` for N >= 3, and its `-2` and `-1` are `--fast=1` and `--fast=2`). The two `--fast` rows are v1.6.11-pre's fast levels: v1.6.12-pre's are about 1 point smaller (`--fast=2` 32.56%, `--fast=1` 30.45% on Silesia) and encode 20% slower, see [NEWS](NEWS).

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
- [NEWS](NEWS) — what changed in each release, including measured costs and refuted ideas
- `lib/entropy/lz6seq.h` — the seq codec API
- [Wiki](https://github.com/YadeWira/lz6/wiki) — architecture, seq stream format, decoder internals, development workflow, roadmap

## Status

Experimental: the seq block format can change between `-pre` versions without a compatibility shim (see [NEWS](NEWS)); v1.6.10-pre did not change it, v1.6.11-pre does (tighter length-code widths: streams written by earlier builds do not decode). The frame HC levels (`--hc`) are stable. The seq codec and the frame layer are fuzz-tested (corrupt raw streams + corrupted frames through the full `LZ6F_decompress` path), and the multithreaded paths were run under AddressSanitizer, UBSan and ThreadSanitizer. Windows binaries (win64 and win32) are VM-tested on Windows 7 SP1 and Windows 10.

[LZ5 v1.5]: https://github.com/inikep/lz5
[zstd]: https://github.com/facebook/zstd
[lizard]: https://github.com/inikep/lizard
[misa77]: https://github.com/welcome-to-the-sunny-side/misa77
[lzbench]: https://github.com/inikep/lzbench
