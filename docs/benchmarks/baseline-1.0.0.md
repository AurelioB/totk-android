# Performance baseline: TotK Native 1.0.0

The reference point for the next round of performance work. Every later measurement should use the same devices,
profiles, scenes and method, and be compared against these numbers. Raw per-window data is in
[`baseline-1.0.0.json`](baseline-1.0.0.json) (schema `totk-android-benchmark/1`).

Measured 2026-11-03 on the release APK.

| Build | Value |
|---|---|
| App | 1.0.0 (versionCode 10000), game 1.4.3 |
| Commit | `edc526df8c95` |
| APK sha256 | `393576ab9b01b9c2370b5dd515b941e0e0a851c763d16b5b624092aab9f68ad6` |
| Turnip driver sha256 | `9b87afeb3d8d30c6a0ad5aab3e0703bc1500e0321e9f17a922106fa556389ec6` |

## Devices and profiles

Each device ran its **default profile** (the preset the app detects and marks "Recommended", no custom changes, no
`totk.env`). The exact `settings.env` of each device is recorded in the JSON.

| Device | SoC | GPU | Android | Default profile | Screens |
|---|---|---|---|---|---|
| AYN Thor | QCS8550 (Snapdragon 8 Gen 2 class) | Adreno 740 | 13 | Balanced: 30 fps, native render resolution | dual |
| AYN Odin 3 | CQ8725S (Snapdragon 8 Elite class) | Adreno 830 | 15 | 60 fps, `TOTK_GPU_TIME=hardware` | single |
| Retroid Pocket Duo | CQ7790S (Snapdragon 7 Gen 4 class) | Adreno 722 | 16 | Light: 30 fps, 960x540, high shadows | dual |

Frame generation is off in every default profile.

## Scenes

From the merged save set used on all three devices:

| Scene | Save slot | Notes |
|---|---|---|
| Hateno Village | `slot_00` | outdoors, in the village |
| Lookout Landing | `slot_02` | arrives **inside** the shelter next to Monte (indoor) |
| Rotsumamu Shrine | `slot_04` | outdoors at the shrine |

## Method

1. Start the game from the app's **Start game** (this rewrites `settings.env` from the profile).
2. Load the slot from the in-game **Load** list; wait about 55 s for the world to settle.
3. Pan the camera horizontally: right stick held at half deflection (value 16000 of 32767), right for half the time,
   then left, injected with `sendevent` on the device's controller node (Thor `event9`, Odin 3 and Duo `event8`).
4. Count only the runtime's own 300-frame windows that start after the stick is pushed and end before it is released.
5. During the pan, sample every 3 s: max CPU and GPU thermal-zone temperature, thermal cooling-device states, the GPU's
   `thermal_pwrlevel`, and each cpufreq policy's `scaling_max_freq` / `cpuinfo_max_freq`.
6. Check `dmesg` for GPU page faults and `totk.log` for "device lost".

Where the numbers come from (`totk.log`):

| Metric | Log line |
|---|---|
| fps | `[nvn-vk] frame N: X fps over the last 300` |
| Frame time p50 / p95 / max | `计时 至帧 N： 帧时间 p50 … / p95 … / 最大 … ms` |
| GPU ms per frame | `[nvn-vk] frame N, per frame: … GPU X ms` |
| Frames over 33 / 50 ms | `[nvn-vk] frame N, stutter: …` |
| Long frames | `[长帧]` lines |

## Results (camera panning)

Ranges are min–max across the windows; long frames and frames over 50 ms are totals across the windows.

| Device | Scene | Windows | fps | p50 ms | p95 ms | Worst ms | GPU ms | ≥45 ms | >50 ms | Max CPU / GPU °C |
|---|---|---|---|---|---|---|---|---|---|---|
| Thor | Hateno | 4 | 30.0–30.1 | 33.14–33.42 | 37.1–38.9 | 91.5 | 22.9–23.9 | 3 | 1 | 87.6 / 67.1 |
| Thor | Lookout Landing | 4 | 30.0 | 33.19–33.31 | 38.1–39.9 | 47.7 | 22.3–22.6 | 1 | 0 | 92.7 / 70.2 |
| Thor | Rotsumamu Shrine | 4 | 30.0 | 32.93–33.21 | 38.2–38.9 | 43.7 | 19.1–21.1 | 0 | 0 | 89.2 / 66.3 |
| Odin 3 | Hateno | 7 | 33.9–55.5 | 17.93–31.64 | 20.2–37.6 | 57.0 | 13.3–16.9 | 1 | 1 | 49.8 / 45.1 |
| Odin 3 | Lookout Landing | 6 | 45.2–55.8 | 17.85–20.58 | 20.0–35.1 | 72.3 | 13.2–15.2 | 1 | 1 | 54.0 / 47.5 |
| Odin 3 | Rotsumamu Shrine | 7 | 59.9 | 16.44–16.73 | 18.7–19.2 | 26.9 | 9.8–11.2 | 0 | 0 | 51.3 / 46.7 |
| Duo | Hateno | 3 | 27.6–29.0 | 33.33–35.36 | 43.0–46.9 | 117.1 | 23.9–25.0 | 8 | 22 | 71.1 / 65.8 |
| Duo | Lookout Landing | 4 | 28.4–29.9 | 33.04–33.88 | 37.9–49.5 | 213.1 | 22.7–23.7 | 15 | 34 | 78.3 / 69.9 |
| Duo | Rotsumamu Shrine | 4 | 29.9 | 33.39–33.58 | 37.0–37.8 | 58.5 | 15.4–17.4 | 1 | 1 | 76.3 / 67.4 |

No GPU page faults, SMMU faults or device lost in any run. No thermal throttling engaged on any device (cooling states
and `thermal_pwrlevel` stayed at 0).

## Caveats

- **Odin 3 CPU cap.** Its CPU clocks were capped at 2.23 / 2.25 GHz (hardware max 3.53 / 4.32 GHz) for the whole
  session. The cap was not thermal (temperatures under 55 °C). Android's performance mode was off, and a ClusterTune
  "Performance" quick-settings tile was present. The Odin 3's Hateno and Lookout Landing results are limited by that
  cap: the GPU needed only 13–17 ms per frame. Record the clock limits with every new Odin 3 measurement, and compare
  only runs with the same limits.
- **Lookout Landing is indoors** in this save. An outdoor Lookout Landing scene would need a different save.
- **Long-frame counts are lower bounds.** The runtime logs a long frame only when it exceeds 20 ms and is 40% above
  the recent 16-frame average. At 30 fps, 45–46.7 ms frames are not counted, and during a sustained slowdown nothing
  is logged. For the 30 fps devices the ">50 ms" column from the stutter line is the more reliable count.
- **Big spikes are in submit + present.** The worst frames (Duo Lookout Landing 213 ms, Duo Hateno 117 ms, Thor
  Hateno 91.5 ms, Odin 3 Lookout Landing 72.3 ms) spent almost all their time in submit + present, with no new
  pipelines, shader libraries or texture uploads in those frames.
- Short runs: 3–7 windows of 300 frames per scene, one session per device.
