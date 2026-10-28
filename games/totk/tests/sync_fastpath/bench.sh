#!/bin/sh
# Same workload/compiler for the pinned old runtime, kick-off control and default.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=${1:?pass an absolute .scratch output directory}
case "$OUT" in /*/.scratch/*) ;; *) echo "output must be an absolute path under .scratch" >&2; exit 2;; esac
mkdir -p "$OUT"
BASE=${SYNC_BASELINE_REV:-70d3e68}
git -C "$ROOT" show "$BASE:games/totk/runtime/threads.c" > "$OUT/baseline-threads.c"
CC=${CC:-cc}
COMMON="-D_GNU_SOURCE -DDM_THREADS_HOST_TEST -DSYNC_BENCH_ONLY -std=gnu11 -O2 -g -Wall -Wextra -Werror -Wno-format -Wno-unused-parameter -Wno-unused-function -Wno-misleading-indentation -ffunction-sections -fdata-sections -pthread"
for arm in old new; do
    if [ "$arm" = old ]; then
        $CC $COMMON -DSYNC_LEGACY -DSYNC_RUNTIME="\"$OUT/baseline-threads.c\"" \
            -I"$ROOT/android/compat/include" -I"$ROOT/runtime" -I"$ROOT/third_party/d2r_gfx/gfx/nvn/include" \
            "$ROOT/tests/sync_fastpath/probe.c" "$ROOT/tests/guest_waits/host_support.c" -Wl,--gc-sections -o "$OUT/bench-$arm"
    else
        $CC $COMMON -I"$ROOT/android/compat/include" -I"$ROOT/runtime" -I"$ROOT/third_party/d2r_gfx/gfx/nvn/include" \
            "$ROOT/tests/sync_fastpath/probe.c" "$ROOT/tests/guest_waits/host_support.c" -Wl,--gc-sections -o "$OUT/bench-$arm"
    fi
done
for repeat in 1 2 3 4 5; do
    case "$repeat" in 2|4) arms="on off old";; *) arms="old off on";; esac
    for arm in $arms; do
        echo "ARM=$arm REPEAT=$repeat BASE=$BASE stats=0"
        case "$arm" in
            old) TOTK_SYNC_STATS=0 "$OUT/bench-old" "${SYNC_ROUNDS:-30000}";;
            off) TOTK_SYNC_STATS=0 TOTK_SYNC_KICK=0 "$OUT/bench-new" "${SYNC_ROUNDS:-30000}";;
            on) TOTK_SYNC_STATS=0 TOTK_SYNC_KICK=1 "$OUT/bench-new" "${SYNC_ROUNDS:-30000}";;
        esac
    done
done
