#!/usr/bin/env python3
"""seeds.py BUILD_DIR CLI OUT_DIR FILE...: seed corpora for the five harnesses from slices (1.5, 20 and 60 KB, at the start and at 1 MB) of
the given files: raw seq streams at levels -2..13 (mkseq), plain blocks at levels 0..15 (mkblock), frames made by the lz6 CLI with many
options (behind the control byte fz_frame_dec reads), and raw inputs for the two round-trip harnesses. OUT_DIR gets c_seq_dec, c_block_dec,
c_frame_dec, c_seq_rt and c_frame_rt."""
import os, random, subprocess, sys
B, CLI, OUT = sys.argv[1:4]; files = sys.argv[4:]
random.seed(20261024); slices = []
for fn in files:
    sz = os.path.getsize(fn)
    for ln in (1500, 20000, 60000):
        for off in (0, min(1 << 20, max(0, sz - ln))):
            if sz >= ln + off:
                with open(fn, 'rb') as f: f.seek(off); slices.append(f.read(ln))
for d in ('seq_dec', 'block_dec', 'frame_dec', 'seq_rt', 'frame_rt'): os.makedirs(f'{OUT}/c_{d}', exist_ok=True)
tmp = f'{OUT}/s.bin'
def wr(d, name, b): open(f'{OUT}/c_{d}/{name}', 'wb').write(b)
FRAMES = [['-0', '-B1'], ['--fast=2', '-B1'], ['--fast=1', '-B2'], ['-1', '-B1'], ['-3', '-B1'], ['-5', '-B2', '--content-size'], ['-8', '-B1'],
          ['-10', '-B1'], ['--hc', '-3', '-B1'], ['--hc', '-9', '-B2', '--no-frame-crc'], ['-4', '-B1', '--no-frame-crc']]
for i, b in enumerate(slices):
    open(tmp, 'wb').write(b)
    for lv in [-2, -1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13]:
        if lv >= 11 and len(b) > 20000: continue
        if subprocess.run([f'{B}/mkseq', str(lv), tmp, tmp + '.o'], capture_output=True).returncode == 0: wr('seq_dec', f's{i}_{lv}', open(tmp + '.o', 'rb').read())
    for lv in (0, 1, 3, 6, 9, 12, 15):
        if subprocess.run([f'{B}/mkblock', str(lv), tmp, tmp + '.o'], capture_output=True).returncode == 0: wr('block_dec', f'b{i}_{lv}', open(tmp + '.o', 'rb').read())
    for j, opts in enumerate(FRAMES):
        if len(b) > 20000 and j % 3: continue
        r = subprocess.run([CLI] + opts + ['-c', tmp], capture_output=True)
        if r.returncode == 0 and r.stdout:
            wr('frame_dec', f'f{i}_{j}_a', bytes([0x3e]) + r.stdout); wr('frame_dec', f'f{i}_{j}_b', bytes([random.randrange(256)]) + r.stdout)
    wr('seq_rt', f'r{i}', bytes([random.randrange(16)]) + b[:20000])
    wr('frame_rt', f'g{i}', bytes([random.randrange(256) for _ in range(8)]) + b[:30000])
for x in (tmp, tmp + '.o'):
    if os.path.exists(x): os.unlink(x)
print({d: len(os.listdir(f'{OUT}/c_{d}')) for d in ('seq_dec', 'block_dec', 'frame_dec', 'seq_rt', 'frame_rt')})
