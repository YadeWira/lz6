# lz6 against the LZMA family, zpaq and RAR

Since 2026-10-18 the main yardstick of the project is the LZMA family (xz, lzma, fast-lzma2, uf-lzma2) and zpaq (7.15 and zpaqfranz), with RAR as a closed-source reference: lz6 aims at their ratio with a much faster decode. This file keeps the three reports as measured, on the same machine (2x Xeon E5-2697A v4), the same 19 files (12 Silesia + AIT A, B, C, E, F, G, H; AIT D, the regenerated `random()` file, is left out) and the same method (one thread pinned to one core, lzbench in memory for the library codecs, whole-process wall clock through the command line for zpaq, zpaqfranz and RAR). The lzbench rows of lz6 are the library (`LZ6_compress_seq`) on whole files.

**Level labels.** Report 1 was measured with v1.6.11-pre and uses its level numbers: its `lz6 -1` is `--fast=2` in v1.6.12-pre (32.56% on Silesia there, 33.09% here). Report 2 used a v1.6.12-pre build with the new numbering, from before the byte-delta lanes: its `-13` is 24.98% in the released v1.6.12-pre (25.12% in the table); the decode speed of `-13` did not move beyond noise (1,109 -> 1,099 MB/s in the A/B of [V1612_VS_V1611.md](V1612_VS_V1611.md)). Report 3 (RAR) lists the same files with the lz6 sizes of report 2.

Not here: the encode speed of the zpaq family beyond `-m1`/`-m5` (zpaq -m5 is symmetric, about 0.35 MB/s), and any level between the first and the top ones of the competitors.

## 1. First level of every codec (2026-10-18)

Machine: 2x Xeon E5-2697A v4, one thread pinned to CPU 44 (socket 0), load 2-3. Files: 12 Silesia + 7 AIT (A B C E F G H), compressed one by one. lz6 = v1.6.11-pre (commit b921eb7; the lzbench plugin still labels it 1.6.3-pre); uf-lzma2 = release v1.6.3 sources (lzbench label says 1.1.0: glue text only); fast-lzma2 1.0.1; xz 5.8.3; lzma = LZMA SDK 26.01 (lzbench codec lzma); zstd 1.5.7; zpaq 7.15; zpaqfranz v65.4m (2026-09-24).
First level = 1 for lz6, zstd, fast-lzma2, uf-lzma2, zpaq/zpaqfranz -m1; xz and lzma start at 0, so both -0 and -1 are shown.
lzbench rows: in-memory, 3 passes (codec order forward, reversed, forward), best speed per codec and file; CLI rows: whole process, files in /dev/shm, best of 3 x 2 rounds. Speeds = total bytes / total time. Sizes are exact bytes.

### lzbench, in memory, one thread, CPU 44 (best of 3 passes)

| codec | Silesia % | enc MB/s | dec MB/s | AIT % | enc MB/s | dec MB/s |
|---|---:|---:|---:|---:|---:|---:|
| lz6 1.6.3-pre seq -1 | 33.09 | 163.6 | 1028 | 47.90 | 197.2 | 1107 |
| zstd 1.5.7 -1 | 34.55 | 355.3 | 1179 | 52.71 | 385.8 | 1190 |
| xz 5.8.3 -0 | 29.71 | 20.5 | 85 | 49.99 | 12.3 | 52 |
| xz 5.8.3 -1 | 27.70 | 17.0 | 97 | 48.81 | 10.9 | 56 |
| lzma 26.01 -0 | 28.56 | 24.9 | 69 | 49.20 | 17.0 | 42 |
| lzma 26.01 -1 | 27.58 | 21.9 | 74 | 48.96 | 14.4 | 44 |
| fastlzma2 1.0.1 -1 | 27.84 | 20.1 | 73 | 47.86 | 15.8 | 43 |
| uf-lzma2 1.1.0 -1 | 27.84 | 20.2 | 104 | 47.86 | 15.9 | 63 |

### CLI wall clock (whole process, files in tmpfs), one thread, CPU 44, best of 3 x 2 rounds

| codec | Silesia % | enc MB/s | dec MB/s | AIT % | enc MB/s | dec MB/s |
|---|---:|---:|---:|---:|---:|---:|
| zpaq | 32.86 | 22.1 | 78 | 60.84 | 19.6 | 54 |
| zpaqfranz | 32.86 | 29.4 | 72 | 60.84 | 23.2 | 50 |
| lz6_1 | 32.92 | 108.2 | 367 | 47.90 | 102.5 | 105 |
| zstd_1 | 34.57 | 196.7 | 548 | 52.71 | 152.8 | 292 |
| xz_0 | 29.53 | 17.7 | 85 | 49.95 | 10.3 | 49 |
| xz_1 | 27.56 | 15.5 | 96 | 48.81 | 9.4 | 51 |
| lzma_0 | 29.53 | 17.8 | 86 | 49.94 | 10.3 | 49 |
| lzma_1 | 27.56 | 15.5 | 97 | 48.80 | 9.5 | 51 |

round-trip failures in the CLI harness: none

### per file, compressed size as % of the original (lz6, zstd, xz, lzma, uf-lzma2 from lzbench; zpaq, zpaqfranz from the CLI)

| file | lz6 -1 | zstd -1 | xz -1 | lzma -1 | uf2 -1 | zpaq m1 | zpaqf m1 |
|---|---:|---:|---:|---:|---:|---:|---:|
| dickens | 39.49 | 41.81 | 36.10 | 34.42 | 35.35 | 37.81 | 37.81 |
| mozilla | 39.31 | 38.99 | 29.67 | 30.03 | 30.16 | 38.99 | 38.99 |
| mr | 34.02 | 38.25 | 31.04 | 31.18 | 30.24 | 38.01 | 38.01 |
| nci | 9.35 | 8.49 | 7.11 | 7.37 | 7.19 | 9.32 | 9.32 |
| ooffice | 50.52 | 58.41 | 43.48 | 43.94 | 43.68 | 47.20 | 47.20 |
| osdb | 35.21 | 36.98 | 33.84 | 32.63 | 32.75 | 39.98 | 39.98 |
| reymont | 32.37 | 32.43 | 27.97 | 26.37 | 28.52 | 30.93 | 30.93 |
| samba | 26.61 | 25.40 | 21.17 | 21.29 | 21.75 | 23.99 | 23.99 |
| sao | 82.50 | 86.18 | 67.95 | 66.94 | 66.34 | 80.51 | 80.51 |
| webster | 31.99 | 32.95 | 27.40 | 26.85 | 28.11 | 28.68 | 28.68 |
| xml | 14.20 | 12.97 | 10.52 | 10.52 | 11.43 | 13.18 | 13.18 |
| x-ray | 57.45 | 79.91 | 58.97 | 60.09 | 57.34 | 74.27 | 74.27 |
| ait_A | 52.86 | 52.41 | 57.02 | 58.15 | 55.36 | 82.35 | 82.35 |
| ait_B | 25.35 | 25.69 | 20.87 | 20.40 | 21.76 | 23.97 | 23.97 |
| ait_C | 33.04 | 33.68 | 28.14 | 28.63 | 27.48 | 34.90 | 34.91 |
| ait_E | 79.76 | 88.47 | 86.45 | 86.48 | 85.89 | 100.27 | 100.27 |
| ait_F | 79.44 | 87.78 | 85.28 | 85.29 | 82.71 | 100.18 | 100.18 |
| ait_G | 29.23 | 38.11 | 32.69 | 32.60 | 31.92 | 43.67 | 43.67 |
| ait_H | 46.28 | 49.95 | 38.44 | 38.40 | 38.24 | 48.89 | 48.89 |

## 2. Top levels (2026-10-19)

Same method as BASELINE_L1.md: 2x Xeon E5-2697A v4, one thread pinned to CPU 44, 12 Silesia + 7 AIT files compressed one by one. lz6 = v1.6.12-pre renumbered (`renumber-levels`, 7eb4301; the lzbench label says 1.6.3-pre); uf-lzma2 = v1.6.3 sources (label 1.1.0); lzbench rows in memory, best of 2 passes; zpaq 7.15 and zpaqfranz v65.4m through the CLI (-m5 -t1, whole process, tmpfs, ONE round). Speeds = total bytes / total time. lz6 numbers: new level N = old level N+2.

| codec | Silesia % | enc MB/s | dec MB/s | AIT % | enc MB/s | dec MB/s |
|---|---:|---:|---:|---:|---:|---:|
| lz6 -13 | 25.12 | 0.7 | 1118 | 45.36 | 1.5 | 1096 |
| lz6 -10 | 25.22 | 2.9 | 1141 | 45.48 | 5.5 | 1115 |
| lz6 -6 | 26.99 | 8.4 | 1052 | 45.86 | 9.5 | 1070 |
| lz6 -1 | 28.77 | 61.4 | 1099 | 46.70 | 83.4 | 1140 |
| xz -9 (lzma2) | 23.02 | 2.7 | 115 | 45.92 | 3.3 | 57 |
| lzma -9 | 23.00 | 2.8 | 88 | 46.00 | 4.4 | 44 |
| fast-lzma2 -10 | 22.97 | 3.9 | 90 | 45.50 | 5.8 | 40 |
| uf-lzma2 -10 | 22.97 | 4.0 | 124 | 45.50 | 5.7 | 55 |
| uf-lzma2 -11 | 22.88 | 1.3 | 123 | 44.13 | 1.9 | 55 |
| zpaq -m5 (CLI) | 18.45 | 0.35 | 0.35 | 40.79 | 0.37 | 0.36 |
| zpaqfranz -m5 (CLI) | 18.45 | 0.35 | 0.35 | 40.79 | 0.37 | 0.37 |

round-trip failures in the CLI part: none

### per file, size as % of the original

| file | lz6 -13 | xz -9 | lzma -9 | fl2 -10 | uf2 -11 | zpaq -m5 | zpaqf -m5 |
|---|---:|---:|---:|---:|---:|---:|---:|
| dickens | 28.29 | 27.77 | 27.78 | 28.07 | 28.05 | 20.55 | 20.55 |
| mozilla | 30.14 | 26.11 | 26.05 | 26.14 | 25.87 | 23.51 | 23.51 |
| mr | 32.71 | 27.58 | 27.56 | 27.52 | 27.47 | 21.88 | 21.88 |
| nci | 5.33 | 5.18 | 5.19 | 4.43 | 4.41 | 3.73 | 3.73 |
| ooffice | 38.15 | 39.45 | 39.42 | 39.62 | 39.46 | 28.71 | 28.71 |
| osdb | 30.71 | 28.26 | 28.27 | 28.33 | 28.33 | 21.86 | 21.86 |
| reymont | 20.86 | 19.87 | 19.89 | 20.10 | 20.06 | 14.43 | 14.43 |
| samba | 19.43 | 17.42 | 17.40 | 17.39 | 17.36 | 14.13 | 14.13 |
| sao | 69.90 | 60.88 | 60.86 | 61.52 | 61.45 | 53.77 | 53.77 |
| webster | 20.61 | 20.23 | 20.23 | 20.39 | 20.37 | 13.67 | 13.67 |
| xml | 9.34 | 8.48 | 8.51 | 8.18 | 8.17 | 6.12 | 6.12 |
| x-ray | 55.80 | 52.98 | 52.86 | 52.57 | 52.53 | 43.30 | 43.30 |
| ait_A | 52.87 | 53.34 | 53.75 | 51.87 | 51.87 | 51.30 | 51.30 |
| ait_B | 18.38 | 17.76 | 17.73 | 17.78 | 17.78 | 13.14 | 13.14 |
| ait_C | 27.03 | 24.72 | 24.74 | 24.78 | 24.59 | 18.10 | 18.11 |
| ait_E | 79.45 | 85.84 | 85.70 | 85.52 | 78.01 | 76.53 | 76.53 |
| ait_F | 79.45 | 82.38 | 82.71 | 82.53 | 79.12 | 76.39 | 76.39 |
| ait_G | 29.22 | 29.71 | 29.61 | 28.53 | 28.51 | 26.47 | 26.47 |
| ait_H | 38.69 | 35.87 | 35.85 | 35.97 | 35.86 | 30.27 | 30.28 |

## 3. RAR 7.23 (2026-10-19)

RAR 7.23 (Linux trial, closed source, CLI only) at `-m1`, `-m3`, `-m5`, one thread (`-mt1`), pinned to CPU 44, files and archives on tmpfs, 3 rounds, best per file; same 12 Silesia + 7 AIT files as TOP_LEVELS.md. Speeds are whole-process wall clock (compress; `unrar x` for decode), so the decode of the fast levels and of the small AIT files is understated by process start. All 57 round trips OK. Reference columns come from TOP_LEVELS.md (lz6 -13 from lzbench, in memory).

| codec | Silesia % | enc MB/s | dec MB/s | AIT % | enc MB/s | dec MB/s |
|---|---:|---:|---:|---:|---:|---:|
| RAR (-m1) | 31.46 | 40.2 | 144 | 51.16 | 28.0 | 87 |
| RAR (-m3) | 25.58 | 7.8 | 158 | 48.67 | 10.3 | 73 |
| RAR (-m5) | 25.07 | 4.7 | 159 | 48.62 | 6.6 | 71 |
| lz6 -13 | 25.12 | 0.7 | 1118 | 45.36 | 1.5 | 1096 |
| xz -9 (lzma2) | 23.02 | 2.7 | 115 | 45.92 | 3.3 | 57 |
| uf-lzma2 -11 | 22.88 | 1.3 | 123 | 44.13 | 1.9 | 55 |
| zpaq -m5 (CLI) | 18.45 | 0.35 | 0.35 | 40.79 | 0.37 | 0.36 |

### per file, size as % of the original

| file | lz6 -13 | RAR -m1 | RAR -m3 | RAR -m5 | xz -9 | uf2 -11 | zpaq -m5 |
|---|---:|---:|---:|---:|---:|---:|---:|
| dickens | 28.29 | 35.69 | 30.58 | 29.75 | 27.77 | 28.05 | 20.55 |
| mozilla | 30.14 | 35.86 | 29.71 | 29.59 | 26.11 | 25.87 | 23.51 |
| mr | 32.71 | 35.53 | 27.86 | 27.78 | 27.58 | 27.47 | 21.88 |
| nci | 5.33 | 8.91 | 6.21 | 5.54 | 5.18 | 4.41 | 3.73 |
| ooffice | 38.15 | 52.10 | 37.53 | 37.42 | 39.45 | 39.46 | 28.71 |
| osdb | 30.71 | 35.41 | 32.83 | 32.09 | 28.26 | 28.33 | 21.86 |
| reymont | 20.86 | 29.61 | 23.60 | 21.80 | 19.87 | 20.06 | 14.43 |
| samba | 19.43 | 22.56 | 19.20 | 19.01 | 17.42 | 17.36 | 14.13 |
| sao | 69.90 | 76.98 | 76.50 | 76.43 | 60.88 | 61.45 | 53.77 |
| webster | 20.61 | 28.47 | 22.75 | 21.79 | 20.23 | 20.37 | 13.67 |
| xml | 9.34 | 12.76 | 9.27 | 9.07 | 8.48 | 8.17 | 6.12 |
| x-ray | 55.80 | 76.24 | 49.56 | 49.10 | 52.98 | 52.53 | 43.30 |
| ait_A | 52.87 | 52.57 | 57.21 | 57.21 | 53.34 | 51.87 | 51.30 |
| ait_B | 18.38 | 22.99 | 18.97 | 18.80 | 17.76 | 17.78 | 13.14 |
| ait_C | 27.03 | 30.34 | 27.84 | 27.77 | 24.72 | 24.59 | 18.10 |
| ait_E | 79.45 | 88.62 | 87.46 | 87.44 | 85.84 | 78.01 | 76.53 |
| ait_F | 79.45 | 87.97 | 84.84 | 84.84 | 82.38 | 79.12 | 76.39 |
| ait_G | 29.22 | 36.14 | 33.15 | 33.11 | 29.71 | 28.51 | 26.47 |
| ait_H | 38.69 | 46.87 | 38.19 | 38.15 | 35.87 | 35.86 | 30.27 |
