# Benchmark tools

Host-side scripts (POSIX sh and Python 3 standard library) for measuring TotK Native on rooted Android devices over adb
and comparing against the recorded baseline. The full procedure is the `totk-benchmark` skill
(`.omp/skills/totk-benchmark/SKILL.md`); this is the quick reference.

Device side: `su` root (for `sendevent`, the app's private `totk.log`, `settings.env` and `dmesg`). Nothing is pushed
to the device; the only device write is the dmesg marker of `collect.sh --mark`.

Common options: `-s <serial>` (adb serial), `--device <label>` (a short name used in file names, e.g. `thor`),
`--package` (default `com.aure.totk`), `--game-dir` (default `/data/user/0/<package>/files/game`).

| Script | Use |
|---|---|
| `bench_input.sh -s <serial> [--event /dev/input/eventN] [--display <id>] <action>` | `press <A\|B\|X\|Y\|L\|R\|PLUS\|MINUS\|UP\|DOWN\|LEFT\|RIGHT> [ms]`, `rstick <x> [y]` (`rstick 0` releases), `shot <file>`, `log`, `detect`. Auto-detects the controller node (`getevent -p`: ABS_Z/ABS_RZ + BTN_SOUTH) and the first display (`dumpsys SurfaceFlinger --display-id`). |
| `pan.sh -s <serial> --device <label> --scene <label> --seconds N [--value 16000] --out <dir>` | Steady pan: stick right for N/2 s, left for N/2 s. Appends `<device> <scene> F0= F1= T0= T1= V=` to `<dir>/pans.txt`, mid-pan screenshot in `<dir>/shots/`. The game must be running. |
| `sampler.py -s <serial> --device <label> --out <dir> [--duration S]` | Every 3 s: newest frame, max CPU/GPU zone temperature, cooling states, GPU `thermal_pwrlevel`/clock, cpufreq `scaling_max/cpuinfo_max` → `<dir>/samples-<label>.txt`. Stops on Ctrl-C / SIGTERM / `--duration` and removes its device loop. |
| `collect.sh -s <serial> --device <label> --out <dir> [--mark]` | `--mark`: dmesg marker (after the scene settles). Default: pulls `logs/<label>.log`, `logs/<label>-dmesg.txt` (from the marker), `settings-<label>.env`, `facts-<label>.txt` (model, SoC, GPU, Android, displays, app version, APK sha256, serial hash), warns about a developer `totk.env`, prints the GPU-fault check. Exit 3 when there is no `totk.log`. |
| `analyze.py <dir> [--json <file>] [--name <name>] [--commit <sha>] [--meta <json>]` | Per device × scene: the 300-frame windows that started after the stick was pushed and ended before it was released (end frame in `[F0+600, F1]`), thermal state over each pan, a results table and caveats; `--json` writes schema `totk-android-benchmark/1` (the shape of `docs/benchmarks/baseline-1.0.0.json`). |
| `compare.py <baseline.json> <new.json>` | Per device × scene: fps mean/min, p50 median, p95 max, worst frame, GPU ms mean, long ≥45 ms, >50 ms, max temperatures, with deltas and verdicts (thresholds in `compare.py --help`); warns when `settings_env` or cpufreq caps differ. Exit 1 if any scene is worse. |
| `ledger.py add <result.json> --commit <sha> --apk <apk> [--note ...]` | Append a run to `docs/benchmarks/ledger.jsonl` (+ `docs/benchmarks/runs/<run-id>/<device>.json`), tied to the clean commit the APK was built from (its `assets/build.txt`), compared with the current baseline and the previous entry of the same device model and settings; re-renders `docs/benchmarks/LEDGER.md`. Also `render`, `show <run-id>`, `compare <run-id> <run-id>`. |
| `check_baselines.py [--pin <file>]` | Fails if a `docs/benchmarks/baseline-*` file differs from `docs/benchmarks/BASELINES.sha256` or is unpinned (also run by `tests/test_bench_tools.py`). |

Baselines are immutable: `analyze.py`/`compare.py` refuse to write `docs/benchmarks/baseline-*` unless
`--new-baseline` is given, and never overwrite an existing one. The current baseline is named in
`docs/benchmarks/CURRENT_BASELINE`.

Example (one device, one scene; placeholders in `<>`):

```sh
OUT=.scratch/bench/<run-name>
tools/bench/collect.sh -s <serial> --device <label> --out $OUT --mark     # after the scene settled
python3 tools/bench/sampler.py -s <serial> --device <label> --out $OUT &  # keep running for the whole session
tools/bench/pan.sh -s <serial> --device <label> --scene hateno --seconds 50 --out $OUT
kill %1                                                                    # stop the sampler
tools/bench/collect.sh -s <serial> --device <label> --out $OUT
python3 tools/bench/analyze.py $OUT --commit <sha> --json $OUT/result.json
python3 tools/bench/compare.py docs/benchmarks/baseline-1.0.0.json $OUT/result.json
```
