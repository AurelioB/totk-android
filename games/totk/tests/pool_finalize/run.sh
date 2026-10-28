#!/bin/sh
# MIT. Real Vulkan + production guest scheduler; synthetic memory only.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=${1:?pass an absolute .scratch output directory}
case "$OUT" in /*/.scratch/*) ;; *) echo "output must be under .scratch" >&2; exit 2;; esac
make -C "$ROOT/android" -j4 -s poolfintest TARGET=linux CC="${CC:-clang}" OUT="$OUT"
for arm in default protected; do
    unset TOTK_FRAME_LAG TOTK_VK_REC_THREADS
    if [ "$arm" = protected ]; then export TOTK_FRAME_LAG=1 TOTK_VK_REC_THREADS=2; fi
    for release in 0 1; do
        for stats in 0 1; do
            TOTK_VCORE_RELEASE=$release TOTK_POOL_STATS=$stats timeout 120 "$OUT/poolfintest" "${POOL_ROUNDS:-30}"
        done
    done
    POOL_SHUTDOWN=1 TOTK_VCORE_RELEASE=1 TOTK_POOL_STATS=0 timeout 30 "$OUT/poolfintest"
done
