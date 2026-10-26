#!/bin/sh
# Run from any directory; all compiler outputs stay in the supplied .scratch.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=${1:?pass an absolute .scratch output directory}
case "$OUT" in /*/.scratch/*) ;; *) echo "output must be an absolute path under .scratch" >&2; exit 2;; esac
mkdir -p "$OUT"
CC=${CC:-cc}
COMMON="-D_GNU_SOURCE -DDM_THREADS_HOST_TEST -std=gnu11 -O2 -g -Wall -Wextra -Werror -Wno-format -Wno-unused-parameter -Wno-unused-function -Wno-misleading-indentation -ffunction-sections -fdata-sections -pthread"
for game in totk botw; do
    case "$game" in totk) GAME_DEF=;; botw) GAME_DEF=-DGAME_BOTW;; esac
    "$CC" $COMMON $GAME_DEF -I"$ROOT/android/compat/include" -I"$ROOT/runtime" -I"$ROOT/third_party/d2r_gfx/gfx/nvn/include" \
        "$ROOT/tests/core_wait_stats/probe.c" "$ROOT/tests/guest_waits/host_support.c" -Wl,--gc-sections -o "$OUT/core-waits-$game"
    # Separate processes exercise the real per-translation-unit atomic caches.
    TOTK_CORE_DIAG=0 DM_NVN_TSV= "$OUT/core-waits-$game" disabled
    TOTK_CORE_DIAG=0 DM_NVN_TSV=enabled "$OUT/core-waits-$game" tsv
    TOTK_CORE_DIAG=1 DM_NVN_TSV= "$OUT/core-waits-$game" diag
done
