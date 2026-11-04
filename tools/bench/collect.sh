#!/bin/sh
# collect.sh -s <serial> --device <label> --out <dir> [--mark] [--package <pkg>] [--game-dir <dir>]
#
# --mark   write a benchmark marker line into the kernel log (dmesg) and exit; do this after the scene has settled.
# default  pull, into <out>/:
#            logs/<label>.log          the game's totk.log
#            logs/<label>-dmesg.txt    dmesg from the last marker of this label on (the whole buffer if none)
#            settings-<label>.env      the app's files/settings.env (what the menu / profile wrote at game start)
#            totk-env-<label>.txt      only if a developer <game>/totk.env exists (then the run is NOT a default profile)
#            facts-<label>.txt         key=value device and build facts (model, SoC, Android, GPU, app version, APK sha256)
#          and print a fault check (GPU page / SMMU faults in dmesg, "device lost" in totk.log).
# Nothing is written to the device except the kernel-log marker. Exits 3 if totk.log could not be pulled.
set -u

usage() { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
die() { echo "collect: $*" >&2; exit 1; }

SER=""; DEV=""; OUT=""; MARK=0; PKG=com.aure.totk; GAME=""
while [ $# -gt 0 ]; do
  case $1 in
    -s) SER=${2:?}; shift 2 ;;
    --device) DEV=${2:?}; shift 2 ;;
    --out) OUT=${2:?}; shift 2 ;;
    --mark) MARK=1; shift ;;
    --package) PKG=${2:?}; shift 2 ;;
    --game-dir) GAME=${2:?}; shift 2 ;;
    -h|--help) usage ;;
    *) die "unknown argument $1" ;;
  esac
done
[ -n "$SER" ] && [ -n "$DEV" ] && [ -n "$OUT" ] || usage
case $DEV in *[!A-Za-z0-9_-]*) die "--device: letters, digits, _ and - only" ;; esac
[ -n "$GAME" ] || GAME=/data/user/0/$PKG/files/game
SETTINGS=/data/user/0/$PKG/files/settings.env
TAG="TOTK_BENCH_MARK $DEV"

adbs() { adb -s "$SER" "$@"; }
root() { adbs shell "su -c '$1'"; }
adbs get-state >/dev/null 2>&1 || die "device $SER not reachable over adb"
root true >/dev/null 2>&1 || die "no root (su) on $SER"

if [ $MARK = 1 ]; then
  now=$(date +%s)
  root "echo \"$TAG $now\" > /dev/kmsg" || die "cannot write the dmesg marker"
  mkdir -p "$OUT"; echo "$TAG $now" >> "$OUT/marks.txt"
  echo "collect: marked dmesg: $TAG $now"
  exit 0
fi

mkdir -p "$OUT/logs"
status=0
if root "test -f $GAME/totk.log"; then
  root "cat $GAME/totk.log" > "$OUT/logs/$DEV.log"
  echo "collect: $OUT/logs/$DEV.log ($(wc -l < "$OUT/logs/$DEV.log") lines)"
else
  echo "collect: ERROR no $GAME/totk.log on $SER: the game has not run since install / data clear" >&2
  status=3
fi
running=no; adbs shell pidof "$PKG" >/dev/null 2>&1 && running=yes
[ $running = yes ] || echo "collect: note: $PKG is not running (the log is from its last session)" >&2

if root "test -f $SETTINGS"; then
  root "cat $SETTINGS" | tr -d '\r' > "$OUT/settings-$DEV.env"
  echo "collect: $OUT/settings-$DEV.env"
else
  echo "collect: WARNING no $SETTINGS (start the game from the app once)" >&2
fi
if root "test -f $GAME/totk.env"; then
  root "cat $GAME/totk.env" | tr -d '\r' > "$OUT/totk-env-$DEV.txt"
  echo "collect: WARNING $GAME/totk.env exists: developer overrides, this is NOT a default-profile run" >&2
fi

root "dmesg" | tr -d '\r' > "$OUT/logs/$DEV-dmesg.full.tmp"
n=$(grep -n "$TAG [0-9]" "$OUT/logs/$DEV-dmesg.full.tmp" | tail -n1 | cut -d: -f1)
if [ -n "$n" ]; then
  tail -n "+$n" "$OUT/logs/$DEV-dmesg.full.tmp" > "$OUT/logs/$DEV-dmesg.txt"
  echo "collect: $OUT/logs/$DEV-dmesg.txt (from the last marker)"
else
  mv "$OUT/logs/$DEV-dmesg.full.tmp" "$OUT/logs/$DEV-dmesg.txt"
  echo "collect: WARNING no '$TAG' marker in dmesg (run collect.sh --mark first): kept the whole buffer" >&2
fi
rm -f "$OUT/logs/$DEV-dmesg.full.tmp"

prop() { adbs shell getprop "$1" | tr -d '\r'; }
apk=$(adbs shell pm path "$PKG" | tr -d '\r' | sed -n 's/^package://p' | grep '/base\.apk$' | head -n1)
pkginfo=$(adbs shell dumpsys package "$PKG" | tr -d '\r')
gpu=""
[ -f "$OUT/logs/$DEV.log" ] && gpu=$(sed -n 's/^\[nvn-vk\] device Turnip \(.*\), driver .*/\1/p' "$OUT/logs/$DEV.log" | head -n1 | sed 's/ (TM)//')
[ -n "$gpu" ] || gpu=$(adbs shell cat /sys/class/kgsl/kgsl-3d0/gpu_model 2>/dev/null | tr -d '\r')
{
  echo "model=$(prop ro.product.model)"
  echo "manufacturer=$(prop ro.product.manufacturer)"
  echo "soc=$(prop ro.soc.manufacturer) $(prop ro.soc.model)"
  echo "android=$(prop ro.build.version.release)"
  echo "sdk=$(prop ro.build.version.sdk)"
  echo "fingerprint=$(prop ro.build.fingerprint)"
  echo "displays=$(adbs shell dumpsys SurfaceFlinger --display-id | tr -d '\r' | grep -c '^Display [0-9]')"
  echo "gpu=$gpu"
  echo "app_version=$(printf '%s\n' "$pkginfo" | sed -n 's/^ *versionName=//p' | head -n1)"
  echo "version_code=$(printf '%s\n' "$pkginfo" | sed -n 's/^ *versionCode=\([0-9]*\).*/\1/p' | head -n1)"
  echo "apk_sha256=$( [ -n "$apk" ] && adbs shell sha256sum "$apk" | tr -d '\r' | cut -d' ' -f1)"
  echo "game_running=$running"
  echo "serial_sha256=$(printf '%s' "$SER" | sha256sum | cut -c1-16)"
  echo "collected_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$OUT/facts-$DEV.txt"
echo "collect: $OUT/facts-$DEV.txt"

if [ -f "$OUT/logs/$DEV.log" ]; then
  faults=$(grep -ciE 'page fault|pagefault|smmu.*fault|context fault|gpu fault|adreno.*fault' "$OUT/logs/$DEV-dmesg.txt")
  lost=$(grep -ci 'device lost' "$OUT/logs/$DEV.log")
  echo "collect: fault check: dmesg GPU/SMMU fault lines $faults, totk.log 'device lost' lines $lost"
fi
exit $status
