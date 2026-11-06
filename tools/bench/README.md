# Benchmark tools

Host-side scripts (POSIX sh and Python 3 standard library) for measuring TotK Native on rooted Android devices over adb
and comparing against the recorded baselines. The full procedure is the `totk-benchmark` skill
(`.omp/skills/totk-benchmark/SKILL.md`); this is the quick reference.

Device side: `su` root (for `sendevent`, the app's private `totk.log` and `settings.env`, `dmesg`, and the thermal /
cpufreq / kgsl sysfs files). Nothing is pushed to the device; the device writes are the dmesg marker of
`collect.sh --mark` and the live-switch control file of `live.sh` / `ab.sh` (`<game>/totk.ctl`, written with the game
folder's owner and `chcon --reference=<game dir>`). Everything is read from `totk.log` and sysfs / procfs, never from
logcat (some devices' logd drops the game's lines). `bench_input.sh`, `pan.sh` and `live.sh` run every root command
through one persistent root shell per device (`rootsh.py`; `sampler.py` holds its own), pinned to the slowest cpufreq
cluster: a `su -c` per command costs the device a su session and, with Magisk, a su-notify `app_process` on any core.

Common options: `-s <serial>` (adb serial), `--device <label>` (a short name used in file names, e.g. `thor`),
`--package` (default `com.aure.totk`), `--game-dir` (default `/data/user/0/<package>/files/game`).

| Script | Use |
|---|---|
| `bench_input.sh -s <serial> [--event /dev/input/eventN] [--display <id>] <action>` | `press <A\|B\|X\|Y\|L\|R\|PLUS\|MINUS\|UP\|DOWN\|LEFT\|RIGHT> [ms]`, `rstick <x> [y]` (`rstick 0` releases), `shot <file>`, `log`, `detect`, `freqs` (the frequency limits as one line: `cpuN=<cpus>:<scaling_min>:<scaling_max>:<cpuinfo_max>` kHz per cpufreq policy, `gpu_maxclk`, `gpu_freqs`, `gpu_devfreq_max`, GPU power levels). Auto-detects the controller node (`getevent -p`: ABS_Z/ABS_RZ + BTN_SOUTH) and the first display (`dumpsys SurfaceFlinger --display-id`). |
| `pan.sh -s <serial> --device <label> --scene <label> --seconds N [--value 16000] [--first right\|left] [--no-shot] --out <dir>` | Steady pan: stick right for N/2 s, left for N/2 s (`--first left`: the other way round), so the view returns. Appends `<device> <scene> F0= F1= T0= T1= V= D=` to `<dir>/pans.txt` and the frequency limits before / after the pan to `<dir>/freqs.txt`; after releasing the stick (outside the measured windows) a screenshot in `<dir>/shots/` unless `--no-shot`. The game must be running. |
| `rootsh.py -s <serial> run [-c <script>] \| status \| stop` | The serial's persistent root shell: a host-side broker holding one `adb shell su -c sh`, started by the first `run`, ended by `stop`, after 30 idle minutes or when the device goes. `run` executes the script (default: stdin) in its own `sh -c` and passes its stdout, stderr and status through (124: no answer in time, 125: no root shell). The shell pins itself to the slowest cpufreq cluster (`status` prints the cores). |
| `live.sh -s <serial> set KEY=VALUE \| unset KEY \| mark <text>...` | Live switches (APP.md *Live switches*): appends the lines to `<game>/totk.ctl` in one write (one generation at the next presented frame) and prints the game's `[totk-ctl]` answer (a mark's acknowledgement, refusals); exit 3 when a mark is not acknowledged, 4 when a line was refused. |
| `ab.sh -s <serial> --device <label> --scene <label> --arms 'A:<lines>;B:<lines>' [--order ABBAABBA] [--pan-seconds 30] [--settle 15] --out <dir>` | In-session A/B in one loaded scene: per block, the arm's lines plus `mark ab arm=<X> block=<n> scene=<scene> direction=<right\|left>` (live.sh), a settle, a pan (direction alternating). Writes `<dir>/arms.txt` with the direction recorded. |
| `sampler.py -s <serial> --device <label> --out <dir> [--duration S]` | Every 3 s: newest frame, max CPU/GPU zone temperature, cooling states, GPU `thermal_pwrlevel`/clock/`max_gpuclk`, cpufreq `scaling_max/cpuinfo_max`, MemAvailable, the game's VmRSS / VmSwap / RssShmem, its kgsl memory (`/sys/class/kgsl/kgsl/proc/<pid>/{kernel,gpumem_mapped,imported}`), PSI cpu / memory some / full avg10 and kswapd CPU % → `<dir>/samples-<label>.txt` (values a device lacks stay empty → null). Stops on Ctrl-C / SIGTERM / `--duration` and removes its device loop. |
| `collect.sh -s <serial> --device <label> --out <dir> [--mark]` | `--mark`: dmesg marker (after the scene settles). Default: pulls `logs/<label>.log`, `logs/<label>-dmesg.txt` (from the marker), `settings-<label>.env`, `facts-<label>.txt` (model, SoC, GPU, Android, displays, app version, APK sha256, serial hash), warns about a developer `totk.env`, prints the GPU-fault check. Exit 3 when there is no `totk.log`. |
| `analyze.py <dir> [--json <file>] [--name <name>] [--commit <sha>] [--meta <json>] [--arms]` | Per device × scene: the 300-frame windows that started after the stick was pushed and ended before it was released (end frame in `[F0+600, F1]`), thermal state over each pan, the frequency limits of each pan and whether they stayed put (`freq_stable`), each device's frequency signature, a results table and caveats; `--json` writes schema `totk-android-benchmark/1`. `--arms`: each window goes to the live-switch generation that covered it whole and that generation's arm (windows straddling a change are dropped); per arm pooled metrics, per block metrics and the same-arm spread (`ab` in the JSON). With the sampler's memory fields, each window, run and arm also gets `memory`: MemAvailable minimum and the maxima of the rest. |
| `compare.py <reference.json> <new.json>` | Per device × scene: fps mean/min, p50 median, p95 max, worst frame, GPU ms mean, long ≥45 ms, >50 ms, max temperatures, with deltas and verdicts (thresholds in `compare.py --help`); labels each device pair by frequency signature: match / reference only / NOT COMPARABLE; warns when `settings_env` or cpufreq caps differ or caps changed during a run. Exit 1 if a scene with a matching signature is worse. |
| `abtest.py <ab.json> [--ref A]` | Each arm of an `analyze.py --arms` result against the reference: pooled and same-direction ABBA paired deltas, per-direction deltas, sign agreement, exact sign p-value, tolerance, informational same-arm spread and verdict. Better/worse requires both deltas beyond tolerance plus unanimous pair signs (or p < 0.05 with ≥6 pairs); paired-only crossings are informational leans. Prefer ABBAABBA or longer at 60 fps; full rule in `--help` / the skill. Exit 1 if an arm is worse. |
| `ledger.py add <result.json> --commit <sha> --apk <apk> --profile <text> [--note ...]` | Append a run to `docs/benchmarks/ledger.jsonl` (+ `docs/benchmarks/runs/<run-id>/<device>.json`), tied to the clean commit the APK was built from (its `assets/build.txt`); compared with the baseline of its frequency signature, or, for a signature seen for the first time (with stable caps), recorded as that signature's NEW baseline (`docs/benchmarks/baselines/<device>/<signature>.json`, pinned, indexed); re-renders `docs/benchmarks/LEDGER.md`. Also `render`, `show <run-id>`, `compare <run-id> <run-id>`. |
| `check_baselines.py` | Fails if a baseline file (`docs/benchmarks/baseline-*`, `docs/benchmarks/baselines/**`) differs from `docs/benchmarks/BASELINES.sha256` or is unpinned, or `docs/benchmarks/baselines/index.json` is inconsistent (also run by `tests/test_bench_tools.py`). |

Baselines are immutable and per frequency signature (device model + SoC, settings.env hash, per-cluster CPU caps,
GPU cap), listed in `docs/benchmarks/baselines/index.json`. Only `ledger.py add` creates one (for a new signature);
`analyze.py`/`compare.py` refuse to write any baseline path. `baseline-1.0.0.json` is registered there without a GPU
cap, so it gives reference-only comparisons.

Example (one device, one scene; placeholders in `<>`):

```sh
OUT=.scratch/bench/<run-name>
tools/bench/bench_input.sh -s <serial> freqs                               # the caps you are about to measure
tools/bench/collect.sh -s <serial> --device <label> --out $OUT --mark     # after the scene settled
python3 tools/bench/sampler.py -s <serial> --device <label> --out $OUT &  # keep running for the whole session
tools/bench/pan.sh -s <serial> --device <label> --scene hateno --seconds 48 --out $OUT
kill %1                                                                    # stop the sampler
tools/bench/collect.sh -s <serial> --device <label> --out $OUT
python3 tools/bench/analyze.py $OUT --commit <sha> --json $OUT/result.json
python3 tools/bench/ledger.py add $OUT/result.json --commit <sha> --apk <apk> --profile "<preset> (default)" --note "<why>"
```

## GPU pass profile (host-only analysis)

`python3 tools/bench/gpuprof.py 'capture/gpuprof-*.jsonl*' [--json analysis.json]` reads schema-2
GPU-profiler rows, including the three `.1`/`.2`/`.3` rotations. `--json -` emits JSON only on stdout.
The per-pass table retains the exact key and ordinal; families combine only the same kind, sample count and
attachment slot/format/exact normalized width and height/load/store structure, ignoring shader identity.
Times and counters are **summed before dividing**, never averages of producer ratios or per-frame ratios.
Unavailable statistics stay unavailable. Time/draw ratios use only complete-stat rows so their denominators
match; partial-stat rows still contribute their available counters to counter-to-counter ratios.

The report includes measured/span/gap sums, absent frame IDs within the captured range, missing window summaries,
pass/window sum mismatches, dropped/failed slots, partial windows and malformed/incomplete/duplicate/conflicting
rows. Separate launch filenames remain separate observations; repeated rotation rows count once, while conflicting
identities are excluded rather than choosing an arbitrary value. Complete missing leading/trailing windows cannot
be inferred. `long_frame` tables describe selected, capped rows only; they are never added to window totals or
treated as an unbiased latency distribution. Exit 1 means malformed/conflicting rows; a partial final JSON line
is a reported capture warning. Exit 2 means an input/output error.

`tests/test_gpuprof.py` pins the schema, ratios, optional statistics, rotation and long-frame accounting without
Vulkan. The optional `GPUPROF_TEST_BIN=<absolute vkgpuproftest path>` enables fresh-process real Vulkan runs
for modes 0/1/2/3, recorder workers 0/2, statistics on/off/unsupported, split-frame reconciliation and bounded
file rotation. Build `vkgpuproftest` via `games/totk/android/Makefile`; it also builds the existing `vkdraw-seed`.
Set `TOTK_SHADER_SEED_DIR` to that seed's absolute directory and select the host lavapipe loader as for `vkdrawtest`.
The fixture verifies actual pixels and secondary recorder use; it does not establish Android overhead or speedup.
Run host builds/matrices under `flock /home/agent/Projects/totk-android/.scratch/hosttest.lock`
and `taskset -c 0-3 nice -n 10`; no Android device is needed.
