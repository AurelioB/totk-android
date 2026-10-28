#!/bin/sh
# Real recording mutex + scheduler; no Vulkan device, guest image, or synthetic lock implementation.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=${1:?pass an absolute .scratch output directory}
case "$OUT" in /*/.scratch/*) ;; *) echo "output must be under .scratch" >&2; exit 2;; esac
mkdir -p "$OUT"
CC=${CC:-cc}
COMMON="-D_GNU_SOURCE -DDM_THREADS_HOST_TEST -DGPU_VK -DTOTK_VK_TESTS -std=gnu11 -O2 -g -Wall -Wextra -Werror -Wno-format -Wno-unused-parameter -Wno-unused-function -Wno-misleading-indentation -Wno-missing-field-initializers -ffunction-sections -fdata-sections -pthread"
$CC $COMMON -I"$ROOT/android/compat/include" -I"$ROOT/runtime" -I"$ROOT/third_party/d2r_gfx/gfx/nvn" -I"$ROOT/third_party/d2r_gfx/gfx/nvn/include" \
    "$ROOT/tests/vcore_release/probe.c" "$ROOT/tests/guest_waits/host_support.c" \
    "$ROOT/android/gfx/nvn_vk.c" "$ROOT/android/gfx/host_test_support.c" \
    "$ROOT/android/gfx/vk_pcache.c" \
    -Wl,--gc-sections -ldl -lm -o "$OUT/vcore-release"
for release in 0 1; do
    for stats in 0 1; do
        TOTK_VCORE_RELEASE=$release TOTK_VCORE_STATS=$stats timeout 90 "$OUT/vcore-release" "${VCORE_ROUNDS:-1000}"
    done
done
