#!/bin/sh
# Live-switch channel (runtime/live_ctl.h) on the host, plain and under ThreadSanitizer (when the compiler has it).
# Pass an absolute output directory under .scratch.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=${1:?pass an absolute .scratch output directory}
case "$OUT" in /*/.scratch/*) ;; *) echo "output must be an absolute path under .scratch" >&2; exit 2;; esac
mkdir -p "$OUT/plain" "$OUT/tsan"
CC=${CC:-cc}
FLAGS="-D_GNU_SOURCE -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-misleading-indentation -pthread -I$ROOT/runtime"
$CC $FLAGS "$ROOT/tests/live_ctl/probe.c" -o "$OUT/live-ctl"
"$OUT/live-ctl" core "$OUT/plain"
if $CC $FLAGS -fsanitize=thread "$ROOT/tests/live_ctl/probe.c" -o "$OUT/live-ctl-tsan" 2>/dev/null; then
    TSAN_OPTIONS="halt_on_error=1 exitcode=66" "$OUT/live-ctl-tsan" core "$OUT/tsan"
else
    echo "skip: $CC has no -fsanitize=thread"
fi
