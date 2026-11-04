#!/usr/bin/env python3
"""sampler.py -s <serial> --device <label> --out <dir> [--interval 3] [--duration S] [--package PKG] [--game-dir DIR]

Every --interval seconds, through one persistent root shell on the device (nothing is pushed), print the newest
'计时 至帧 N' frame of totk.log, the max CPU / GPU thermal-zone temperature, the sum of the thermal cooling-device states
(cpufreq / cpu-cluster / kgsl / gpu), the GPU's thermal_pwrlevel, clock and max_gpuclk (its effective cap), each
cpufreq policy's scaling_max_freq/cpuinfo_max_freq, and memory pressure: MemAvailable, the game's VmRSS / VmSwap /
RssShmem (kB), its kgsl memory /sys/class/kgsl/kgsl/proc/<pid>/{kernel,gpumem_mapped,imported} (bytes, whichever
exist), PSI cpu / memory some / full avg10 (/proc/pressure), and the kswapd threads' CPU ticks, from which the host adds
kswapd's CPU % over the interval. Lines are host-timestamped (epoch seconds) and appended to <out>/samples-<label>.txt:

  <epoch> f=<frame> cpu=<mC> gpu=<mC> cool=<sum> tpl=<level> gclk=<Hz> gcap=<Hz> mav=<kB> rss=<kB> swp=<kB> shm=<kB>
          kgk=<B> kgm=<B> kgi=<B> pcs=<avg10> pcf=<avg10> pms=<avg10> pmf=<avg10> kst=<ticks> ksw=<%> caps= <scaling>/<cpuinfo> ...

A value the device does not expose (no matching thermal zone / cooling device, no kgsl, no PSI, the game not running)
is left empty and becomes null.

Runs until --duration elapses, or SIGINT / SIGTERM; then the device loop and its sleep are killed, so no process is left
on the device. As a safety net the device loop also ends by itself after --max-seconds (default 3 h, or --duration + 60).
(The loop sleeps with `sleep`: mksh's `read -t` under su overshoots its timeout by seconds on some devices.) The loop
pins itself, and so every command it starts, to the slowest cpufreq cluster (rootsh.PIN_SH); the cleanup goes through
the serial's persistent root shell (rootsh.py), so sampling costs two su sessions in all."""
import argparse, os, signal, subprocess, sys, threading, time

import rootsh

LOOP = r'''
echo "pid=$$ cpus=$C"
END=$(( $(date +%s) + @MAX@ ))
cd /sys/class/thermal 2>/dev/null
CZ=""; GZ=""; CD=""
for z in thermal_zone*; do t=$(cat $z/type 2>/dev/null); case $t in cpu*) CZ="$CZ $z/temp";; gpuss*) GZ="$GZ $z/temp";; esac; done
for c in cooling_device*; do t=$(cat $c/type 2>/dev/null); case $t in cpufreq-*|cpu-cluster*|thermal-cluster*|kgsl|gpu|devfreq-*kgsl*) CD="$CD $c/cur_state";; esac; done
L=@LOG@
PSI() { sed -n "s/^$1 avg10=\([0-9.]*\).*/\1/p" $2 2>/dev/null </dev/null; }
while true; do
  f=$(tail -c 30000 $L 2>/dev/null | grep -ao '至帧 [0-9]*' | tail -n1 | grep -o '[0-9]*')
  # </dev/null: with an empty zone list `cat` would otherwise read this script's stdin and block.
  ct=$(cat $CZ 2>/dev/null </dev/null | sort -n | tail -n1); gt=$(cat $GZ 2>/dev/null </dev/null | sort -n | tail -n1)
  cs=""; [ -z "$CD" ] || { cs=0; for s in $(cat $CD 2>/dev/null </dev/null); do cs=$((cs+s)); done; }
  caps=""; for p in /sys/devices/system/cpu/cpufreq/policy*; do caps="$caps $(cat $p/scaling_max_freq 2>/dev/null)/$(cat $p/cpuinfo_max_freq 2>/dev/null)"; done
  P=$(pidof @PKG@ 2>/dev/null | cut -d' ' -f1); S=/proc/${P:-0}/status; K=/sys/class/kgsl/kgsl/proc/${P:-0}
  st() { [ -n "$P" ] && grep -m1 "^$1:" $S 2>/dev/null </dev/null | tr -s ' \t' ' ' | cut -d' ' -f2; }
  kv() { [ -n "$P" ] && cat $K/$1 2>/dev/null </dev/null; }
  ks=""; for k in $(pgrep kswapd 2>/dev/null); do t=$(cut -d' ' -f14,15 /proc/$k/stat 2>/dev/null </dev/null); [ -n "$t" ] && ks=$(( ${ks:-0} + ${t% *} + ${t#* } )); done
  mem="mav=$(grep -m1 '^MemAvailable:' /proc/meminfo </dev/null | tr -s ' ' | cut -d' ' -f2) rss=$(st VmRSS) swp=$(st VmSwap) shm=$(st RssShmem) kgk=$(kv kernel) kgm=$(kv gpumem_mapped) kgi=$(kv imported)"
  psi="pcs=$(PSI some /proc/pressure/cpu) pcf=$(PSI full /proc/pressure/cpu) pms=$(PSI some /proc/pressure/memory) pmf=$(PSI full /proc/pressure/memory) kst=$ks"
  echo "f=${f:-0} cpu=$ct gpu=$gt cool=$cs tpl=$(cat /sys/class/kgsl/kgsl-3d0/thermal_pwrlevel 2>/dev/null) gclk=$(cat /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null) gcap=$(cat /sys/class/kgsl/kgsl-3d0/max_gpuclk 2>/dev/null) $mem $psi caps=$caps"
  [ $(date +%s) -lt $END ] || exit 0
  sleep @INTERVAL@
done
'''


class Stop(Exception):
    pass


USER_HZ = 100   # /proc/<pid>/stat ticks per second on Linux / Android


def add_kswapd(line, now, previous):
    """The device line with ksw=<kswapd CPU % since the previous sample> inserted before caps= (empty for the first
    sample or without kst), and the new (time, ticks) state."""
    ticks = next((t[4:] for t in line.split(' caps=', 1)[0].split() if t.startswith('kst=')), '')
    ksw = ''
    if ticks.isdigit():
        if previous and now > previous[0] and int(ticks) >= previous[1]:
            ksw = f'{(int(ticks) - previous[1]) / USER_HZ / (now - previous[0]) * 100:.1f}'
        state = (now, int(ticks))
    else:
        state = None
    head, sep, tail = line.partition(' caps=')
    return f'{head} ksw={ksw}{sep}{tail}', state


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('-s', dest='serial', required=True)
    ap.add_argument('--device', required=True, help='device label used in file names (e.g. thor)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--interval', type=int, default=3)
    ap.add_argument('--duration', type=float, help='stop after this many seconds (default: until SIGINT/SIGTERM)')
    ap.add_argument('--max-seconds', type=int, help='device-side safety deadline (default 10800, or --duration + 60)')
    ap.add_argument('--package', default='com.aure.totk')
    ap.add_argument('--game-dir', help='default /data/user/0/<package>/files/game')
    a = ap.parse_args()
    game = a.game_dir or f'/data/user/0/{a.package}/files/game'
    os.makedirs(a.out, exist_ok=True)
    path = os.path.join(a.out, f'samples-{a.device}.txt')

    def stop(*_):
        raise Stop
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    if a.duration:
        signal.signal(signal.SIGALRM, stop)
        signal.setitimer(signal.ITIMER_REAL, a.duration)

    # Own session: a terminal Ctrl-C must reach only this script, which then stops the device loop in order.
    p = subprocess.Popen(['adb', '-s', a.serial, 'shell', 'su -c sh'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         text=True, start_new_session=True)
    max_s = a.max_seconds or (int(a.duration) + 60 if a.duration else 10800)
    p.stdin.write(rootsh.PIN_SH + LOOP.replace('@LOG@', f'{game}/totk.log').replace('@INTERVAL@', str(a.interval))
                  .replace('@PKG@', a.package)
                  .replace('@MAX@', str(max_s)))
    p.stdin.flush()
    remote_pid, n, kswapd = None, 0, None
    try:
        with open(path, 'a', buffering=1) as out:
            for line in p.stdout:
                line = line.rstrip('\r\n')
                if remote_pid is None and line.startswith('pid='):
                    remote_pid, _, cpus = line[4:].partition(' cpus=')
                    print(f'sampler: {a.serial} -> {path} (device loop pid {remote_pid}, cpus {cpus or "unpinned"})',
                          file=sys.stderr)
                    continue
                if not line.startswith('f='):
                    print(f'sampler: device: {line}', file=sys.stderr)
                    continue
                now = time.time()
                line, kswapd = add_kswapd(line, now, kswapd)
                out.write(f'{now:.1f} {line}\n')
                n += 1
    except Stop:
        pass
    finally:
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.setitimer(signal.ITIMER_REAL, 0)
        if remote_pid:   # the loop shell and its current sleep / cat children
            rootsh.run(a.serial, f'kill $(pgrep -P {remote_pid}) {remote_pid}')
        try:
            p.stdin.close()
        except (BrokenPipeError, OSError):
            pass
        reader = threading.Thread(target=lambda: p.stdout.read(), daemon=True)   # drain so adb can exit
        reader.start()
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill(); p.wait()
        if remote_pid:
            left = rootsh.run(a.serial, f'ps -p {remote_pid} -o PID=')[1].decode(errors='replace').strip()
            if left:
                print(f'sampler: WARNING device loop pid {remote_pid} still running', file=sys.stderr)
    print(f'sampler: stopped, {n} samples in {path}', file=sys.stderr)
    if remote_pid is None:
        sys.exit('sampler: the root shell never started (adb / su failed?)')


if __name__ == '__main__':
    main()
