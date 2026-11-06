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

`python3 tools/bench/gpuprof.py 'capture/gpuprof-*.jsonl*' [--json analysis.json]` reads **schema-3 only**
GPU-profiler rows, including the three `.1`/`.2`/`.3` rotations. Normal analysis rejects schema 2 because its
attachment ratios used different reference semantics. `--json -` emits JSON only on stdout (`totk-gpuprof-analysis/3`).

Windows now default to **one row per exact key + reference extent + scene role**, with `ordinal: 0`
meaning **all frame ordinals**, displayed as `all (0)`. `TOTK_VK_GPU_PROFILE_ORDINALS=1` restores
per-ordinal windows (default `0`, read once; startup logs `[gpu-profile] window ordinals=0/1`).
Mode-2 raw `[gpu-split]` logs and mode-3 selected `long_frame` rows always retain their actual positive
frame ordinals. Window `ordinals` records the selected policy; old schema-3 captures without it remain readable.
Counts, times, absolute dimensions and statistics counters/explicitly matched time/draws remain unchanged sums.

Default storage is **128 MiB per file × four files**, overridable with `TOTK_VK_GPU_PROFILE_MAX_BYTES`
(4096–1073741824). Compact mode targets a 6–8-minute pan without rotation, not a duration guarantee.
Each file begins with a `header` containing launch ID, rotation index, build commit from APK `assets/build.txt`,
package/versionCode when available, Vulkan device/driver identity, effective profiling switches and a whitelist
of requested launch environment values (null means unset, not necessarily an effective backend default).
Host fixtures/missing APK metadata explicitly retain unavailable identity; no environment build string is trusted.
Exact APK/driver artifact hashes must be joined from capture records, not inferred from version labels.
Normal Android close/destruction emits `session_end`: `clean_end` proves writer closure, while
`all_queries_retired: null` explicitly does not prove GPU completion. Crash/kill never fabricates this marker.

Capacity model (not a new device-duration measurement): six original Thor S/D/B1 snapshots
(`.scratch/bench/rpmode/thor/{S,D,B1}`, initial and `after-hateno`) contain 77 complete windows,
463,121 per-ordinal pass rows and 11,736 window/key proxies after ordinal coalescing (39.5× fewer).
The largest observed proxy population is 265/window. Serializing those coalesced legacy templates
with an extra 256 bytes/row + 128 bytes/attachment for schema-3 metadata gives a peak 334,477 bytes/window.
At 60 fps for 480 seconds, 700 bytes/new frame × 28,800 frames + 96 peak windows + 64 KiB metadata
is 52,335,328 bytes (49.91 MiB). Schema 2 cannot recover actual schema-3 absolute/reference variants;
allowing **2×** the modeled pass bytes gives 84,445,120 bytes (80.53 MiB), hence the 128 MiB default
rather than a marginal 64 MiB. Arbitrary key/reference churn or raw/long-frame populations can exceed this model.

Pass and selected `long_frame` rows require absolute `render_width`/`render_height`,
`reference_width`/`reference_height`, and boolean `scene_relative`. Every attachment retains its slot, format,
load/store and reduced `width_num`/`width_den`, `height_num`/`height_den`, plus absolute `width`/`height`.
The analyzer checks that each rational exactly equals attachment dimension / reference dimension, not attachment
dimension / pass area. References are the completed frame's largest depth+MRT render area (at least two active
color slots), or the game-config fallback when no such scene is observed: `TOTK_RENDER_RES`, otherwise handheld
1280×720 or docked 1600×900. References and attachment dimensions are positive uint32; render dimensions are
positive uint32 for render scopes and may be paired 0/0 for nonrender scopes. Frame rows require
`scene_width`/`scene_height`: the observed largest depth+MRT render area, or 0/0 when absent.

Exact producer keys hash **absolute** render and attachment dimensions along with scope kind, formats,
load/store, samples and shaders; changing scene size changes its exact key, but not a fixed shadow's key.
Per-pass JSON retains the exact key/ordinal, absolute `render_width`/`render_height` and attachment `width`/`height`
for driver joins. Both pass and family tables expose every contributing `areas` variant with its exact `keys`,
absolute render/attachment dimensions, reference dimensions, attachment rationals and `scene_relative` flag.
Multiple reference extents in one window are distinct observations, not duplicate/conflicting rows, and are
preserved even when their absolute key is the same.

Families ignore shader/key/ordinal but retain kind, samples, slots, formats, load/store and `scene_relative`.
For `scene_relative=true` (depth+MRT geometry, or depth render areas equal to that frame's observed scene),
families compare exact render/reference and attachment/reference ratios, merging matching DRS geometry and
scene-sized depth prepasses across extents. A half-scene MRT remains distinct from a full-scene MRT, including
when both occur in one frame. Fixed depth shadows, color-only passes and all other `scene_relative=false`
scopes compare **absolute** render/attachment dimensions; two depth-only target sizes do not collapse together.
Times and counters are **summed before dividing**, never averages of producer ratios or per-frame ratios.
Unavailable statistics stay unavailable. Time/draw ratios use only complete-stat rows so their denominators
match; partial-stat rows still contribute their available counters to counter-to-counter ratios.

The report includes measured/span/gap sums, absent frame IDs within the captured range, missing window summaries,
complete missing window sequences, rotation eviction/absent files, absent end markers, pass/window sum mismatches,
dropped/failed slots, partial windows and malformed/incomplete/duplicate/conflicting rows. `sessions` retains every
header/end observation. Separate launch filenames remain separate observations; repeated rotation rows count once,
while conflicts are excluded rather than choosing an arbitrary value. Dedup retains exact key, ordinal, reference
dimensions and `scene_relative`, so compact rows do not merge distinct scene roles/references.
Headers establish sequence 1 as the launch start; older headerless captures only prove interior window gaps.
Missing trailing work after an unmarked exit cannot be inferred. A clean writer end is not a flush of pending queries.

Invalid frame/window spans have `span_valid: false` and null `span_ms`/`gap_ms`; reasons, unavailable/invalid
timestamp counters and raw forensic spans remain explicit. The analyzer excludes implausible older positive spans
above 10,000 ms/frame even if the old producer reported success, and propagates invalid frame authority to its window.
`timing` preserves measured sums and valid-span/gap subtotals; total span/gap stay null if that population contains
invalid rows. Invalid timing is not zero GPU time. `rejected_spans` preserves raw values and source locations.

For historical timing-only replay:
`python3 tools/bench/gpuprof.py --legacy-span-replay 'capture/gpuprof-*.jsonl*' --json replay.json`.
This explicit mode accepts schema-2/3 **frame/window timing only**, ignoring pass/long-frame rows and disabling
pass/window sum comparisons. It flags implausible spans without reconstructing query availability/endpoints or
claiming schema-3 attachment/reference semantics. Valid historical timing values remain unchanged.

`long_frame` tables describe over-threshold frames and retained-history diagnostics; they are never added to window
totals or treated as an unbiased latency distribution. `TOTK_VK_GPU_PROFILE_LONG_FRAMES` bounds the history ring,
**not** the number of over-threshold frames dumped; a zero threshold selects every valid frame. Exit 1 means
malformed/conflicting rows; a partial final JSON line is a reported capture warning. Exit 2 means an I/O error.

`tests/test_gpuprof.py` pins schema-3 dimensions/references, scene-relative and fixed-depth families, ratios,
compact ordinal sentinel/dedup/statistics totals, capture-loss lifecycle metadata, legacy span replay, optional
statistics, rotation and long-frame accounting without Vulkan. The optional
`GPUPROF_TEST_BIN=<absolute vkgpuproftest path>` enables fresh-process real Vulkan runs for modes 0/1/2/3,
recorder workers 0/2, statistics on/off/unsupported, ordinal windows off/on, split-frame and unsent multi-frame
reconciliation, direct pre uploads while main rendering is open, default row/byte reduction and bounded file rotation.
Every rotated file must carry its header, and orderly fixture exit must carry its writer-end marker.
`--unavailable-span` deliberately leaves a real span-end query in an unsubmitted command buffer;
the existing submitted buffer's fence retires, but the unsubmitted end remains unavailable.
The matrix requires null invalid frame/window spans, explicit unavailable counters and the orderly writer end,
not fabricated query values or a zero-duration success.
The color-only 64/128 scenario
requires distinct absolute keys and families. Its `--areas` scenario covers changing depth+MRT scenes,
scene-sized prepasses, a smaller MRT later in the same frame, fixed 360×360/128×128 depth targets, and a frame
with no observed scene; all contributing absolute keys and reference variants remain available for joins.
Build `vkgpuproftest` via `games/totk/android/Makefile`; it also builds the existing `vkdraw-seed`.
Set `TOTK_SHADER_SEED_DIR` to that seed's absolute directory and select the host lavapipe loader as for `vkdrawtest`.
The fixture verifies actual pixels and secondary recorder use; it does not establish Android overhead or speedup.
Run host builds/matrices under `flock /home/agent/Projects/totk-android/.scratch/hosttest.lock`
and `taskset -c 0-3 nice -n 10`; no Android device is needed.
