#!/bin/sh
# setup_competitors.sh - rebuild the competitor toolchain that lives in /tmp
# (it dies on every reboot) and restore the AIT corpus to /tmp/ait_train.
#
# Everything is re-downloadable/rebuildable; the only local source of truth
# is bakeoff/corpora/ (the AIT A-H training files + data.zip).
#
# Usage: setup_competitors.sh [ait-dir]     (default /tmp/ait_train)
set -e
cd "$(dirname "$0")/.."

AIT=${1:-/tmp/ait_train}

# --- AIT corpus (secured in bakeoff/corpora) ---
if [ ! -f "$AIT/A" ]; then
    mkdir -p "$AIT"
    cp bakeoff/corpora/[A-H] "$AIT/"
    [ -f bakeoff/corpora/data.zip ] && cp bakeoff/corpora/data.zip "$AIT/" || true
    echo "corpus restored -> $AIT"
fi

# --- lz5 v1.5 from the repo's baseline commit (bench_vs_lz4lz5.sh uses it) ---
if [ ! -x /tmp/lz5src/programs/lz5 ]; then
    rm -rf /tmp/lz5src && mkdir -p /tmp/lz5src
    git archive a8a4ac0 | tar -x -C /tmp/lz5src
    make -C /tmp/lz5src/programs lz5 >/dev/null 2>&1
    echo "lz5 v1.5 rebuilt -> /tmp/lz5src/programs/lz5"
fi

# --- misa77 (cmake, C++20) ---
if [ ! -x /tmp/misa77/build/misa ]; then
    rm -rf /tmp/misa77
    git clone --depth 1 https://github.com/welcome-to-the-sunny-side/misa77 /tmp/misa77 2>&1 | tail -1
    cmake -B /tmp/misa77/build -DCMAKE_BUILD_TYPE=Release -S /tmp/misa77 >/dev/null 2>&1
    cmake --build /tmp/misa77/build >/dev/null 2>&1
    echo "misa77 built -> /tmp/misa77/build/misa"
fi

# --- lizard (lz5 fork with entropy modes) ---
if [ ! -x /tmp/lizard/programs/lizard ]; then
    rm -rf /tmp/lizard
    git clone --depth 1 https://github.com/inikep/lizard /tmp/lizard 2>&1 | tail -1
    make -C /tmp/lizard -j4 >/dev/null 2>&1
    echo "lizard built -> /tmp/lizard/programs/lizard"
fi

# --- lzbench (uniform harness for the externals) ---
if [ ! -x /tmp/lzbench/lzbench ]; then
    rm -rf /tmp/lzbench
    git clone --depth 1 https://github.com/inikep/lzbench /tmp/lzbench 2>&1 | tail -1
    make -C /tmp/lzbench -j4 >/dev/null 2>&1
    echo "lzbench built -> /tmp/lzbench/lzbench"
fi

echo "competitor toolchain ready"
