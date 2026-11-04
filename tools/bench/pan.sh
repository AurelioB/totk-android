#!/bin/sh
# pan.sh -s <serial> --device <label> --scene <label> --seconds N [--value 16000] --out <dir>
#        [--event /dev/input/eventN] [--display <id>] [--package <pkg>] [--game-dir <dir>]
#
# Steady horizontal camera pan: the right stick held at --value (of 32767) to the right for the first half of the time,
# then to the left for the second half, then released. Appends one line to <out>/pans.txt:
#   <device> <scene> F0=<last finished frame window before> F1=<after> T0=<epoch s> T1=<epoch s> V=<value>
# and two lines to <out>/freqs.txt, the frequency limits (`bench_input.sh freqs`) just before and just after the pan:
#   <device> <scene> T0=<epoch s> start|end <cpuN=... gpu_...>
# and takes a mid-pan screenshot into <out>/shots/<device>-<scene>-pan-<T0>.png. The game must be running.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)

usage() { sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
die() { echo "pan: $*" >&2; exit 1; }

SER=""; DEV=""; SCENE=""; SECS=""; V=16000; OUT=""; EV=""; DISP=""; PKG=com.aure.totk; GAME=""
while [ $# -gt 0 ]; do
  case $1 in
    -s) SER=${2:?}; shift 2 ;;
    --device) DEV=${2:?}; shift 2 ;;
    --scene) SCENE=${2:?}; shift 2 ;;
    --seconds) SECS=${2:?}; shift 2 ;;
    --value) V=${2:?}; shift 2 ;;
    --out) OUT=${2:?}; shift 2 ;;
    --event) EV=${2:?}; shift 2 ;;
    --display) DISP=${2:?}; shift 2 ;;
    --package) PKG=${2:?}; shift 2 ;;
    --game-dir) GAME=${2:?}; shift 2 ;;
    -h|--help) usage ;;
    *) die "unknown argument $1" ;;
  esac
done
[ -n "$SER" ] && [ -n "$DEV" ] && [ -n "$SCENE" ] && [ -n "$SECS" ] && [ -n "$OUT" ] || usage
case $DEV$SCENE in *[!A-Za-z0-9_-]*) die "--device / --scene: letters, digits, _ and - only" ;; esac
[ -n "$GAME" ] || GAME=/data/user/0/$PKG/files/game
LOG=$GAME/totk.log

adb -s "$SER" shell pidof "$PKG" >/dev/null 2>&1 || die "$PKG is not running on $SER: start the game and load the scene first"
if [ -z "$EV" ] || [ -z "$DISP" ]; then
  det=$("$HERE/bench_input.sh" -s "$SER" ${EV:+--event "$EV"} ${DISP:+--display "$DISP"} detect) || exit 1
  EV=${det#event=}; EV=${EV%% *}; DISP=${det##*display=}
fi
input() { "$HERE/bench_input.sh" -s "$SER" --event "$EV" --display "$DISP" "$@"; }
lastf() {
  adb -s "$SER" shell "su -c 'grep -ao \"至帧 [0-9]*\" $LOG | tail -n1'" | tr -d '\r' | grep -o '[0-9][0-9]*'
}

F0=$(lastf) || die "no '至帧 N' timing line in $LOG yet (has the game rendered 300 frames?)"
mkdir -p "$OUT/shots"
trap 'input rstick 0' EXIT
trap 'exit 130' INT TERM
FS=$(input freqs) || die "cannot read the frequency limits"
T0=$(date +%s)
input rstick "$V"
sleep "$(awk "BEGIN { print $SECS / 4 }")"
input shot "$OUT/shots/$DEV-$SCENE-pan-$T0.png" >/dev/null || echo "pan: screenshot failed (continuing)" >&2
sleep "$(awk "BEGIN { print $SECS / 4 }")"
input rstick "-$V"
sleep "$(awk "BEGIN { print $SECS / 2 }")"
F1=$(lastf)
T1=$(date +%s)
FE=$(input freqs) || echo "pan: cannot read the frequency limits after the pan" >&2
input rstick 0
trap - EXIT
printf '%s\n' "$DEV $SCENE T0=$T0 start $FS" "$DEV $SCENE T0=$T0 end $FE" >> "$OUT/freqs.txt"
echo "$DEV $SCENE F0=$F0 F1=$F1 T0=$T0 T1=$T1 V=$V" | tee -a "$OUT/pans.txt"
