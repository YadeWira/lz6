# lz6

**lz6** is an experimental lossless compressor: maximum compression with fast decompression.

It started as a fork of [LZ5 v1.5] (itself a LZ4 derivative) and grew into its own codec family:

- **Frame LZ/HC codec** — the classic LZ5-style byte-wise matcher, hardened and tuned.
- **seq engine** — an LZ match finder feeding an FSE/rANS entropy stage, with per-data-type transforms (byte-plane for interleaved floats, PRNG regeneration for pseudo-random data, order-0/order-1/huffman literal coding).
- **LZ6S2** — per-block codec dispatch: every block inside one frame picks its own engine (seq / light-LZ / raw escape).
- **LZ6S3** — content segmentation: the compressor tracks the byte-entropy profile while reading and cuts blocks at content shifts, so each region gets its own literal mode and transform.

## Usage

```
lz6 -2 file              # compress (seq engine, best ratio/speed trade)
lz6 -15 file             # maximum compression
lz6 --hc -9 file         # classic LZ6-HC frame codec
lz6 -d file.lz6          # decompress (codec-agnostic, per-block dispatch)
lz6 -2 -c file | lz6 -d -c     # pipes
lz6 -m file1 file2       # multiple inputs
```

Levels 1-15 select the seq engine; the default level uses the fast LZ frame codec. Blocks up to 256MB, match windows up to 32MB. Frames are self-describing and independently decodable per block.

## Build

Linux/macOS (gcc or clang, C99):

```
make -C programs lz6
```

Windows (mingw-w64 cross-compile from Linux, or MSVC):

```
./build_windows.sh      # win64 + win32, static, Win7 SP1+
```

## Benchmarks

Reference numbers (mem-to-mem, 2x Xeon E5-2697A v4; full matrix + scripts in [bakeoff/](bakeoff/)):

| corpus | mode | ratio | enc MB/s | dec MB/s |
|---|---|---:|---:|---:|
| Silesia.tar | seq L2 | 36.21% | ~99 | ~270 |
| Silesia.tar | seq L15 | 28.34% | ~2 | ~200 |
| Silesia.tar | HC frame -15 | 30.78% | 2.7 | ~994 |
| Silesia.tar | zstd -9 (ref) | 27.87% | 53 | 656 |
| AIT A-H | seq L2 | 42.61% | ~120 | ~300 |
| AIT A-H | seq L15 | 40.82% | ~15 | ~253 |
| AIT A-H | zstd -1 (ref) | 59.91% | 413 | 1,322 |

A complete competitive table (zstd, lizard, misa77, lz4, gzip) with methodology is in [bakeoff/COMPETITORS.md](bakeoff/COMPETITORS.md) and will be refreshed with the upcoming testing-hardware measurements.

## Documentation

- [lz6_Block_format.md](lz6_Block_format.md) — raw block format
- [lz6_Frame_format.md](lz6_Frame_format.md) — frame format (block codec flags, per-block dispatch)
- [bakeoff/COMPETITORS.md](bakeoff/COMPETITORS.md) — competitive analysis vs zstd, lizard, misa77
- `lib/entropy/lz6seq.h` — the seq codec API

## Status

Experimental, format may change. The seq codec and the frame layer are fuzz-tested (corrupt raw streams + corrupted frames through the full `LZ6F_decompress` path); binaries are VM-tested on Windows 7 SP1 x64 and Windows 10.

[LZ5 v1.5]: https://github.com/inikep/lz5
