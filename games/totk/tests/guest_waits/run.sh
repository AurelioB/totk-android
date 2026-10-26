#!/bin/sh
# Run from any directory; pass an absolute output directory under .scratch.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=${1:?pass an absolute .scratch output directory}
case "$OUT" in /*/.scratch/*) ;; *) echo "output must be an absolute path under .scratch" >&2; exit 2;; esac
mkdir -p "$OUT"
CC=${CC:-cc}
COMMON="-D_GNU_SOURCE -DDM_THREADS_HOST_TEST -std=gnu11 -O2 -g -Wall -Wextra -Werror -Wno-format -Wno-unused-parameter -Wno-unused-function -Wno-misleading-indentation -ffunction-sections -fdata-sections -pthread"
# Production thread runtime; only guest-entry/TP and loader linkage are replaced.
"$CC" $COMMON -I"$ROOT/android/compat/include" -I"$ROOT/runtime" -I"$ROOT/third_party/d2r_gfx/gfx/nvn/include" \
    "$ROOT/tests/guest_waits/probe.c" "$ROOT/tests/guest_waits/host_support.c" -Wl,--gc-sections -o "$OUT/guest-waits"
"$OUT/guest-waits"
for probe in thread system-event; do
    case "$probe" in thread) SOURCE=scripts/thread_probe.c;; system-event) SOURCE=tests/system_event_bind/probe.c;; esac
    "$CC" $COMMON -I"$ROOT/android/compat/include" -I"$ROOT/runtime" -I"$ROOT/third_party/d2r_gfx/gfx/nvn/include" \
        "$ROOT/$SOURCE" "$ROOT/runtime/threads.c" "$ROOT/tests/guest_waits/host_support.c" -Wl,--gc-sections -o "$OUT/$probe"
    TOTK_ASYNC_PREEMPT=0 TOTK_READY_CORES=0 TOTK_SVC_PROFILE=0 "$OUT/$probe"
    TOTK_ASYNC_PREEMPT=0 TOTK_READY_CORES=1 TOTK_SVC_PROFILE=1 "$OUT/$probe"
done
