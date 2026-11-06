# Benchmark tools

Host-side scripts (POSIX sh and Python 3 standard library) for measuring TotK Native on rooted Android devices over adb
and comparing against the recorded baselines. The full procedure is the `totk-benchmark` skill
(`.omp/skills/totk-benchmark/SKILL.md`); this is the quick reference.

Device side: `su` root (for `sendevent`, the app's private `totk.log` and `settings.env`, `dmesg`, and the thermal /
cpufreq / kgsl sysfs files). Nothing is pushed to the device; the device writes are the dmesg marker of
`collect.sh --mark` and the live-switch control file of `live.sh` / `ab.sh` (`<game>/totk.ctl`, written with the game
folder's owner and explicit SELinux context from `ls -Zd <game dir>`). Everything is read from `totk.log` and sysfs / procfs, never from
logcat (some devices' logd drops the game's lines). `bench_input.sh`, `pan.sh` and `live.sh` run every root command
through one persistent root shell per device (`rootsh.py`; `sampler.py` holds its own), pinned to the slowest cpufreq
cluster: a `su -c` per command costs the device a su session and, with Magisk, a su-notify `app_process` on any core.

Common options: `-s <serial>` (adb serial), `--device <label>` (a short name used in file names, e.g. `thor`),
`--package` (default `com.aure.totk`), `--game-dir` (default `/data/user/0/<package>/files/game`).

| Script | Use |
|---|---|
| `bench_input.sh -s <serial> [--event /dev/input/eventN] [--display <id>] <action>` | `press <A\|B\|X\|Y\|L\|R\|PLUS\|MINUS\|UP\|DOWN\|LEFT\|RIGHT> [ms]`, `rstick <x> [y]` (`rstick 0` releases), `shot <file>`, `log`, `detect`, `freqs` (the frequency limits as one line: `cpuN=<cpus>:<scaling_min>:<scaling_max>:<cpuinfo_max>` kHz per cpufreq policy, `gpu_maxclk`, `gpu_freqs`, `gpu_devfreq_max`, GPU power levels). Auto-detects the controller node (`getevent -p`: ABS_Z/ABS_RZ + BTN_SOUTH) and the first display (`dumpsys SurfaceFlinger --display-id`). |
| `pan.sh -s <serial> --device <label> --scene <label> [--target-fps 30\|60] [--seconds N] [--repeat N] [--value 16000] [--first right\|left] [--no-shot] --out <dir>` | Return pan: stick right for half the time then left (`--first left`: reverse). Defaults to 48 s at 30 target, 144 s at 60 target. Records frame/time/frequency boundaries and exact GPU cumulative snapshots per pan, then optional screenshot outside motion. |
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

### Pacing states, not just a session average

`analyze.py` uses the preset's `TOTK_FPS60` (`1`: 60 target, `0`: 30 target), recorded pan target, or explicit
`--target-fps 30|60`. Unknown targets remain unclassified, never silently treated as 60. At 60, a window with
at least 60/300 frames in 29–37.5 ms is a dip: complete frame durations take priority, then the complete
present-fence interval histogram. Sparse `[长帧]` / `[frame-tl late]` traces only establish lower bounds; otherwise
the fallback is window fps below 5/6 of target (below 50 for 60). A normal 30-target 33 ms window is steady.
Fence signals are a framework pacing proxy, **not guaranteed hardware scanout** and not proof of controller causality.

The JSON and console separate `fps_steady` (mean of non-dip window fps), `dip_windows`, `dip_time_fraction`
(sum of dip-window seconds / classified-window seconds, using 300/fps), `frames_near33`, and `fps_mean`
(legacy arithmetic window mean). `fps_time_mean` is the actual throughput over those complete windows, i.e.
total frames / total time; neither mean includes warm-up, dropped edge windows or screenshot time. Missing frame
counts remain null; `frames_near33_lower_bound` and coverage describe sparse traces. Do not infer counts from fps or
from the stutter line's >33 ms total, which misses frames just below 33 ms.

A/B comparisons report steady fps and dip time independently; a dip-time difference above 0.05 (five percentage
points) says **verdict confounded by dips** rather than declaring a speed win from mean fps. All-dip arms have no
steady-fps estimate. Historical JSON without pacing data is labelled unavailable and retains legacy comparisons;
re-analyze its original logs with the preset target to obtain a pacing-aware comparison.

**Uncapped mode:** effective `TOTK_UNCAPPED=1` is detected by reading collected `settings-<device>.env` followed by
`totk-env-<device>.txt` overrides; `analyze.py --uncapped` explicitly forces this interpretation for all devices in
an older capture. It does not change the device's settings. Windows are labelled `pacing_state=uncapped` and
summaries report `uncapped=true`, `speed_mode=uncapped`, `dip_metrics_valid=false`; dip counts/fractions and
`fps_steady` are null, not a statement that a 40fps uncapped run is entirely in a dip.
`throughput_fps` is total complete-window frames divided by their summed time (300/fps per window), and is the
primary speed metric in both `compare.py` and paired `abtest.py`. It is also emitted informationally for capped runs.
Uncapped comparisons use frame p50/p95/p99 window maxima, worst frame and >33/>50ms rates as primary tail metrics.
They never apply the dip-confounding veto; mixed capped/uncapped comparisons are inconclusive.

Frame-time tails remain independent of presentation-fence tails: each window retains `p50_ms`, `p95_ms`, and
`p99_ms` from the game's `帧间隔 至帧 ... p99` line (`p99_samples` preserves its sample count). No p99 is invented
when that line is absent, and a present-fence p99 is never substituted. Per-pan/arm/run summaries include
`p50_ms_max`, `p95_ms_max`, `p99_ms_max`, `p99_windows`, `over33_total`, `over50_total`, and `stutter_windows` coverage.
`p99_p50_ratio` per window / `p99_p50_ratio_max` summarize relative frame-time spread; `fps_stddev` is the population
standard deviation across included window fps values. Spread metrics are informational, with throughput and tails
reported separately so identical ~40fps means with different tail spikes do not collapse to one speed-only verdict.

Draws are a **cumulative** backend counter: `draws_per_frame` uses consecutive 300-frame sample differences,
`(D_now - D_previous)/(frame_now - frame_previous)`, never `D_now/300`. First samples, counter/frame resets and
missing intermediate samples have no rate (null); the next complete monotonic pair resumes measurement.
`draws_per_frame_mean` and `draws_windows` report the mean and coverage over usable windows.

### Presentation tails, bins and judder

Each selected window retains `[nvn-vk]` `recording_ms` and the framework present-fence cadence line's
`present_p95_ms` / `present_p99_ms`. Every `pans[].summary`, run summary and live-switch arm/block summary includes
`recording_ms_mean`, `present_p95_ms_max`, `present_p99_ms_max` and their window coverage. The latter are **maxima
of the reported window percentiles**, not reconstructed pooled p95/p99; percentiles cannot be pooled from quantiles.
These fields are informational and do not silently change A/B verdict tolerances.

Always-on AHB bins are `<12.5 / 12.5–20.9 / 20.9–29 / 29–37.5 / 37.5–45.8 / >=45.8 ms`.
`present_interval_bin_counts` sums selected windows' observed pairs; `present_interval_bin_shares` divides by the
total observed pair count, not by 300 or by window count. `present_judder_proxy_fraction` is the **20.9–29 ms bin
share**, only a proxy: the bins cannot recover an exact >16.7 & <30 ms share. Missing histogram coverage is explicit.
Cadence/histogram records are associated with the latest runtime FPS-window marker in the log, as for pacing-state
analysis; asynchronous fence-result blocks are not claimed to be exact physical pan-boundary measurements.

For exact timestamp arithmetic in a **separate diagnostic run**, set `TOTK_PRESENT_CADENCE_TRACE=1` in `totk.env`
for both arms and restart. `[present-trace]` records carry immutable `generation`, transaction `id`, `source_frame`
and `display_ns`. The analyzer sorts/join consecutive IDs within the same generation, requiring both framework fence
timestamps to be present and increasing; it never joins callback order, bridges missing IDs or crosses generation
boundaries. Intervals belong to included frame windows only when both source endpoints lie within that window
(allowing its preceding boundary frame). Duplicate conflicting identities are excluded.
`present_judder_exact_fraction` counts strictly **16,700,000 < delta_ns < 30,000,000**, with explicit
`present_judder_exact_hits` / `present_judder_exact_pairs`: exact over the **observed valid trace pairs**, not an
estimate for missing pairs or proof of physical scanout. It is null without valid traced intervals.
`TOTK_FRAME_TIMELINE=1` logs only selected late per-frame intervals and is not a substitute for this full trace.
Cadence tracing is high-volume and has measurably perturbed Thor timing: keep it off in ordinary performance arms,
do not mix traced/untraced arms, and use independent distinct-picture evidence for displayed smoothness.

### Long pans and exact GPU residency

The target defaults to the active preset (`settings.env`, then developer `totk.env` override); `--target-fps`
overrides detection. Unknown targets use 48 s and omit target metadata rather than incorrectly labelling a 60 run.
For 60-target measurements use `pan.sh --target-fps 60 --out "$OUT" ...`: the default is a continuous **144 s**
pan (72 s each direction), long enough for >=12 complete 300-frame windows even during 30-interval stretches.
`--seconds N` explicitly chooses another length; `--repeat N` runs back-to-back return pans, each with independent
boundaries. `--target-fps 60 --seconds 48 --repeat 3` is the recommended shorter-camera-cycle alternative.
Keep at least 12 **included** windows per session/arm; the analyzer warns for shorter 60-target scene records.
If a prolonged slowdown leaves fewer, lengthen/repeat rather than accepting a short run. Use the same duration,
cycle length and direction schedule in both arms; explicit `ab.sh --pan-seconds 144` keeps its own block schedule.

Every pan invokes `sampler.py --gpu-snapshot` just before and after motion via the persistent root shell, storing raw
cumulative counters in `gpu-time-in-state.jsonl`. The first supported source is KGSL/devfreq `trans_stat`, then the
KGSL `gpu_clock_stats` equivalent (or devfreq `time_in_state`). `analyze.py` reports each pan's frequency counter
deltas and fractions, not a histogram of 3-second instantaneous clock samples. `trans_stat` uses the final residency
column, never its transition matrix; fixed-width concatenated headers are supported. Counter units are preserved
(`time(ms)` means milliseconds; an unspecified unit stays unknown), and frequencies are Hz.
KGSL `gpu_clock_stats` is a vector in `gpu_available_frequencies` order and counts **active clock time**: its fractions
are an active-frequency distribution, not wall-time residency. Boundary reads bracket the pan but include small
host/control latency; timestamps/uptimes and raw text remain available for that audit.
Missing/failed snapshots, source/frequency/unit/boot changes, actual per-frequency counter regressions and
non-advancing counters produce unavailable residency, never fake zeroes. KGSL `reset_count` is **not a devfreq
statistics-reset generation**: Qualcomm's [reset attribute](https://github.com/qualcomm-linux/kgsl/blob/d70a6765a318eda3e400c711587ce373c693c99e/kgsl_pwrctrl.c)
returns `device->reset_counter`, incremented by [Gen8 GPU boot](https://github.com/qualcomm-linux/kgsl/blob/d70a6765a318eda3e400c711587ce373c693c99e/adreno_gen8_gmu.c)
including normal wake after slumber (and recovery). A change alone does not invalidate monotonic residency.
The raw start/end values and `reset_count_delta` remain diagnostic; they do not establish a GPU fault count.
This upstream evidence is not a claim that the Odin's exact vendor kernel source was identified.

For declared `s`/`ms`/`us`/`ns` counters, `time_sanity` records the converted total and device-uptime read bounds.
Reject a total exceeding the elapsed upper bound plus `max(1 s, 5% of elapsed)` (accounting granularity/clock
tolerance). Do not impose a lower bound: devfreq suspension / active-clock counters may legitimately cover less
than wall time. Unknown units retain raw fractions with an explicitly unavailable time-sanity check, never a guessed
unit conversion. Older logs without boundary snapshots cannot recover exact residency.

Sampler `external_power` is **any readable online non-battery kernel supply** (JSON boolean, null when no online
observation exists). It is not Android BatteryService's AC/USB enum: the Odin can expose sysfs `usb=1` while
`dumpsys battery` calls the same external power AC. Each `supply.<name>.type` / `supply.<name>.online` token is
retained under per-pan `power_supplies`; legacy `ac` / `usb` remain raw kernel-type category observations, never
relabeled to match dumpsys. Battery/BMS supplies are excluded from external-power aggregation. `level` is battery
percent. Historical rows derive external_power from available raw ac/usb fields; no readable source stays null.
Pan JSON retains timestamped fields rather than inferring charger state from temperatures.

Android toybox `chcon` does not support GNU `--reference`. `live.sh` reads `CTX=$(ls -Zd "$GAME")`,
extracts its first field (including the full `s0:c...` MLS categories), then runs `chcon "$CTX" "$GAME/totk.ctl"`
before appending. Missing context or failed chcon aborts the write. There is no totk.env writer in these tools;
when writing one manually, apply the same game-directory owner/mode/context procedure to `"$GAME/totk.env"`,
not a generic `app_data_file:s0` label that discards its categories.

Example (one device, one scene; placeholders in `<>`):

```sh
OUT=.scratch/bench/<run-name>
tools/bench/bench_input.sh -s <serial> freqs                               # the caps you are about to measure
tools/bench/collect.sh -s <serial> --device <label> --out $OUT --mark     # after the scene settled
python3 tools/bench/sampler.py -s <serial> --device <label> --out $OUT &  # keep running for the whole session
tools/bench/pan.sh -s <serial> --device <label> --scene hateno --target-fps 60 --seconds 48 --repeat 3 --out $OUT
kill %1                                                                    # stop the sampler
tools/bench/collect.sh -s <serial> --device <label> --out $OUT
python3 tools/bench/analyze.py $OUT --commit <sha> --json $OUT/result.json
python3 tools/bench/ledger.py add $OUT/result.json --commit <sha> --apk <apk> --profile "<preset> (default)" --note "<why>"
```
