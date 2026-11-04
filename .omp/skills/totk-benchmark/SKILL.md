---
name: totk-benchmark
description: Use when measuring TotK Native performance on Android devices, checking a change for performance regressions against the recorded baseline, or recording/reproducing a benchmark ledger entry.
---

# TotK Native benchmark

Measure the game on rooted Android handhelds with the same scenes and method as the recorded baseline, then compare.
All scripts are in `tools/bench/` (paths below are relative to the repository root; quick reference in
`tools/bench/README.md`).

## Baselines are immutable

- The current baseline is **`docs/benchmarks/baseline-1.0.0.md`** + **`docs/benchmarks/baseline-1.0.0.json`**
  (named in `docs/benchmarks/CURRENT_BASELINE`).
- **Never edit, overwrite or regenerate a baseline file.** A new baseline is recorded only when the user explicitly
  asks for one, as a NEW file (`docs/benchmarks/baseline-<version-or-date>.{md,json}`): write the JSON with
  `analyze.py ... --json docs/benchmarks/baseline-<name>.json --new-baseline`, pin both files with
  `python3 tools/bench/check_baselines.py --pin <file>`, then point `docs/benchmarks/CURRENT_BASELINE` at the new JSON and
  update the name in this skill.
- `analyze.py` / `compare.py` refuse to write `docs/benchmarks/baseline-*` without `--new-baseline` and never overwrite
  one. `python3 tools/bench/check_baselines.py` (also in `tests/test_bench_tools.py`) fails when a baseline changed.
- Every measurement round adds its own files: `docs/benchmarks/<name>.md` + `docs/benchmarks/<name>.json` (a short
  write-up in the style of the baseline document plus the `analyze.py` JSON), and a ledger entry (below).

## Prerequisites

- Host: `adb`, POSIX sh, Python 3. Device: root through `su` (sendevent, the app's private files, dmesg).
- The app (`com.aure.totk`) installed and set up, on the device's **default profile**: in the app's Graphics screen the
  preset marked IN USE is the one marked "Recommended for this device" (not Custom), and there is no developer
  `totk.env` (`collect.sh` warns if there is). Start the game with the app's **Start game** button: it rewrites
  `settings.env` from the profile.
- The merged save set (kept outside the repository: `merged-saves.zip` and its sha256 list), imported on the device
  with the app's Saves → Import saves → A zip file → Replace my saves. Scenes:
  | Scene label | Save slot | Note |
  |---|---|---|
  | `hateno` | `slot_00` Hateno Village | outdoors in the village |
  | `lookout` | `slot_02` Lookout Landing | arrives **inside** the shelter next to Monte (indoor) |
  | `rotsumamu` | `slot_04` Rotsumamu Shrine | outdoors at the shrine |
- Nothing else measuring or loading the device (no other samplers, screen recorders, or test loops in `ps -A`).
- Never uninstall the app (it deletes the game data and saves); never change its settings for a benchmark unless the
  run is deliberately on another profile (then say so in the notes).

## Procedure (per device; devices can run in parallel)

Use one output directory per round, e.g. `OUT=.scratch/bench/<YYYY-MM-DD>-<label>` (gitignored), and a short device
label (`thor`, `odin`, `duo`, or a new one for another device).

1. **Check the profile.** Open the app's Graphics screen (`tools/bench/bench_input.sh -s <serial> shot $OUT/shots/<label>-graphics.png`
   and look at it): IN USE must be the Recommended preset. Start the game with **Start game**.
2. **Detect input/display** once: `tools/bench/bench_input.sh -s <serial> detect` prints `event=... display=...`
   (pass them with `--event` / `--display` if detection picks the wrong node).
3. **Load the scene from the in-game Load list** (PLUS menu → System → Load → the slot; navigate with
   `bench_input.sh press <button>` and check each step with `bench_input.sh shot`). Do not use the app's autoload.
4. **Settle 55 s** after the world appears (`shot` the arrival view).
5. **Mark dmesg** (first scene of the session): `tools/bench/collect.sh -s <serial> --device <label> --out $OUT --mark`.
6. **Start the sampler** (first scene; it runs for the whole session):
   `python3 tools/bench/sampler.py -s <serial> --device <label> --out $OUT &`.
7. **Pan**: `tools/bench/pan.sh -s <serial> --device <label> --scene <hateno|lookout|rotsumamu> --seconds 48 --out $OUT`
   (right stick at 16000 of 32767, right then left; 48 s gives at least 4 full windows at 30 fps). Look at the mid-pan
   screenshot in `$OUT/shots/`. Repeat steps 3–4 and 7 for the other scenes.
8. **Stop the sampler** (`kill %1`, or Ctrl-C; it removes its device loop) and **collect**:
   `tools/bench/collect.sh -s <serial> --device <label> --out $OUT`. It must report 0 GPU/SMMU fault lines and 0
   "device lost"; investigate otherwise.
9. **Analyze** all devices of the round:
   `python3 tools/bench/analyze.py $OUT --name <name> --commit <sha> --json $OUT/<name>.json`
   (optionally `--meta <json>` to add curated device names / profile descriptions / extra caveats).
10. **Compare** with the baseline: `python3 tools/bench/compare.py docs/benchmarks/baseline-1.0.0.json $OUT/<name>.json`.
11. **Record**: copy the JSON to `docs/benchmarks/<name>.json`, write `docs/benchmarks/<name>.md` (build, devices and
    profiles, results table printed by `analyze.py`, comparison, caveats), and add the ledger entry (below).
12. **Restore state**: release the stick (`bench_input.sh -s <serial> rstick 0`), stop the game
    (`adb -s <serial> shell am force-stop com.aure.totk`), turn off any screen stay-on you set, delete anything you
    put in the device's Download folder (e.g. save exports), and confirm `ps -A` shows no leftover sampler. If a save
    could have been overwritten (autosave), export the saves from the app and check them against the merged set's
    sha256 list; re-import the merged set if they differ.

## Reading the results

- Windows are the runtime's own 300-frame windows that started after the stick was pushed and ended before it was
  released. `fps`, `p50`/`p95`/`max` frame time, `GPU ms` per frame, `>50 ms` frames (stutter line) and long frames
  (`[长帧]`) come from `totk.log`; temperatures, cooling states, GPU `thermal_pwrlevel` and cpufreq limits from the
  sampler over each pan.
- `compare.py` verdicts: per metric `better` / `worse` / `same` within documented tolerances (`compare.py --help`); a
  scene is worse if fps mean, p50, p95, GPU ms or >50 ms frames (per window) got worse. Exit status 1 when any scene
  is worse.
- **Not comparable** warnings: a different `settings_env` (another profile or menu change) or different cpufreq caps.
  Do not draw conclusions from those pairs.

Known caveats:

- **Odin 3 CPU cap**: in the baseline its CPU clocks were capped at 2.23 / 2.25 GHz (hardware 3.53 / 4.32 GHz) by a
  non-thermal limit (performance mode off, a ClusterTune "Performance" tile present); its Hateno and Lookout Landing
  numbers are CPU-limited. The sampler records the caps; compare only runs with the same caps.
- **Long-frame counts are lower bounds**: a long frame is logged only when over 20 ms and 40 % above the recent
  16-frame average, so 45–46.7 ms frames at 30 fps and sustained slowdowns are missed. For 30 fps devices the
  `>50 ms` count is the reliable one.
- **Lookout Landing is indoors** in this save set (the shelter next to Monte).
- Short runs (3–7 windows per scene): differences inside the tolerances are noise; repeat a run before calling a
  small change.

## Capture a new benchmark (any device)

1. **Build the experiment APK from a clean commit** (no uncommitted changes: the APK's `assets/build.txt` must not say
   `-dirty`, or the ledger refuses it):
   ```sh
   git status --short            # must be empty
   python3 games/totk/android/build_release.py --ndk <Android NDK r28> --turnip <Turnip driver.so> \
     --key <signing key .p12> --key-pass-env <VAR> -o .scratch/bench/<sha12>.apk
   ```
2. **Install** with `adb -s <serial> install -r .scratch/bench/<sha12>.apk`. If it fails with a signature mismatch,
   stop and ask the user (never uninstall: that deletes the game data and saves).
3. **Profile**: the device's default (Recommended) profile, or a deliberate profile written in the ledger note.
4. **Run the procedure above.** Unknown devices are fine: `collect.sh` records their facts (model, SoC, GPU, Android,
   displays, serial hash). They are compared only against earlier ledger entries for the same device model and
   `settings_env`, or against the baseline only if the device is in it.
5. **Add to the ledger**, tied to that commit and APK (the commit must exist; the APK's `assets/build.txt` must match
   it; its sha256 must equal the installed APK's that `collect.sh` recorded):
   ```sh
   python3 tools/bench/ledger.py add $OUT/<name>.json --commit <full sha> --apk .scratch/bench/<sha12>.apk \
     [--run-id <YYYY-MM-DD>-<label>] [--profile "<profile>"] --note "<what changed / why>"
   ```
   This writes `docs/benchmarks/runs/<run-id>/<device>.json`, appends to `docs/benchmarks/ledger.jsonl`, prints the
   verdicts against the current baseline and the previous comparable entry, and re-renders
   `docs/benchmarks/LEDGER.md` (`python3 tools/bench/ledger.py render` does it alone). The ledger is append-only.
6. **Commit** the ledger, LEDGER.md and the run JSON (plus `docs/benchmarks/<name>.{md,json}` for a full round).

## Reproduce a ledger entry

1. `python3 tools/bench/ledger.py show <run-id>`: commit, APK sha256, device, profile, settings and scenes.
2. `git checkout <commit>` (in a clean tree or a worktree), rebuild with the command above, and check the APK's
   sha256 / `assets/build.txt`. (Release builds are not byte-reproducible across keys; the commit is what must match.)
3. Install with `adb install -r`, set the same profile (its `settings_env` must equal the entry's), and run the same
   scenes with the procedure above.
4. Add it as a new ledger entry (new run id) and compare:
   `python3 tools/bench/ledger.py compare <old-run-id> <new-run-id>` (or `compare.py` on the two result JSONs).
