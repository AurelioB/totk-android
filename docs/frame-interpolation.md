# 30 -> 60 fps frame interpolation (Android, opt-in)

Status and design record for `TOTK_FRAMEGEN=interp` on branch `feat/android-frame-interp`. The game keeps running
at a fixed 30 fps; between two real frames we synthesize a midpoint and present it half a tick later, so a 60 Hz
panel shows 60 images per second. The price is one tick (33.3 ms) of extra latency, which was accepted as a
trade-off. Off by default.

## Hybrid native60 / real30+interpolation (Odin 3, opt-in)

`TOTK_FRAMEGEN=hybrid` starts in native60 policy and engages delayed interpolation
only after sustained low real-frame rate. It requires the 1.4.3 FPS60 and measured
realtime patches. It is **not** `TOTK_FPS_AUTO=1`: the hybrid owner is the NVN
acquire thread, and only its mode changes decide whether interpolation is active.
A native frame for which the game briefly chooses interval2 stays native.

The owner leaves the game's controller state, listener veto and completed CPU/GPU
work measurements untouched. In interpolation mode it raises the effective NVN
interval floor to2 before acquisition pacing; the measured realtime frame step
therefore runs simulation/input at real30. Native mode again obeys the guest's
interval, never shortening a deliberate interval2/3 request.

Defaults:

| Variable | Default | Meaning / accepted range |
|---|---:|---|
| `TOTK_HYBRID_DROP_FPS` | `55` | Real game-present rate below this threshold;30..59fps. Not generated-output rate or compositor callbacks. |
| `TOTK_HYBRID_DROP_SECONDS` | `5` | Two consecutive independent windows of this duration must both be slow;1..60s each. |
| `TOTK_HYBRID_RECOVER_SECONDS` | `8` | Completed controller estimates must fit for at least95% of this wall-time window;1..120s. |
| `TOTK_HYBRID_RECOVER_SCORE` | `0.9` | Both `workTicks/320000 + originalBias` scores must be below this value;0.1..1.0. The game's one-tick threshold is1;0.9 reserves headroom. |
| `TOTK_HYBRID_DWELL_SECONDS` | `10` | Minimum normal residence in either mode;1..300s. |

These are seconds, not frame-count thresholds. Recovery never uses the achieved
paced30 rate as evidence that native60 fits. Missing/frozen controller state,
clock reversals and gaps over250ms discard eligibility evidence. Missing/invalid
completed work estimates restart recovery only: with frame lag, a GPU report
minus a newer CPU origin can be negative even while a live world presents at50fps.
That unavailable counterfactual estimate cannot invalidate the independently
observed raw present rate or prove headroom. The existing first-world/draw-relative
scene gate requires10s settling after loading/low-draw scenes. Unsupported guest
intervals or a non60Hz physical clock leave interpolation immediately for safety,
even inside normal dwell. Invalid option values log the range and use the default.

**Transitions and ownership.** Entering interpolation cuts camera/source history
and extraction backoff, warms up an adjacent pair, and adds one33.333ms real tick
of latency. Static interpolation selection and the presenter pool stay in
place during native mode (six slots, seven with opt-in FSR scene scaling), but
scene extraction/private UI replay are disabled.
On native return the never-shown retained real is discarded through FIFO GPU-fence
retirement, not recycled early or shown after its midpoint. The native reservation
skips past any last queued child, removing the delayed picture without temporal
reversal. Already queued slots and children keep their immutable deadlines.
One picture may be omitted at this rare latency transition.
Entering also seeds a fresh full-period real30 display phase, retaining the last
reserved target as a floor. A native60 phase that was already late cannot be
inherited indefinitely by a deliberately paced30 producer; queued pictures keep
their original targets. This is the same initial display lead as fixed interp,
not a changed target for an already queued picture.

The bounded pool still fails closed with `no-buffer` rather than blocking game
production or stealing compositor ownership. Hybrid does not eliminate the
observed Odin21–61 unavailable-child slots per300 real frames or guarantee steady
physical scanout60. Reserve/run-ahead combinations are a later stage: hybrid
requires `TOTK_ACQUIRE_SLACK=0`, `TOTK_ACQUIRE_CLOCK=0`, `TOTK_PACE=0`, and synchronous
`TOTK_ASYNC_REPLAY=0`. Protected frame lag and recorder workers are supported.
Incompatible startup logs `[hybrid] refused:` and retains the prior native policy.
Off, `1`, fixed `interp`, and `FPS_AUTO` retain their existing semantics.

### Odin enablement and qualification

Fix the physical panel at60Hz. Keep the same save, render resolution, driver,
clock/power/thermal conditions and all other play settings in matched arms:

```ini
TOTK_VK_REC_THREADS=2
TOTK_FRAME_LAG=1
TOTK_FPS60=1
TOTK_FPS_AUTO=0
TOTK_UNCAPPED=0
TOTK_REALTIME=1
TOTK_GPU_TIME=hardware
TOTK_PRESENT_SLOTS=1
TOTK_VK_PRESENT=ahb
TOTK_ACQUIRE_SLACK=0
TOTK_ACQUIRE_CLOCK=0
TOTK_PACE=0
TOTK_ASYNC_REPLAY=0
TOTK_NATIVE_UI=0
TOTK_FRAMEGEN=hybrid
TOTK_FRAMEGEN_DIAG=1
```

Leave `TOTK_FRAMEGEN_BUDGET_MS` unset for the existing adaptive GPU headroom guard.
The above explicitly disables the default acquire slack. Do not reuse a reserve2
play environment. Compare against the same environment with only
`TOTK_FRAMEGEN=0`; compare fixed real30 separately with `FRAMEGEN=interp FPS60=0`.

Native UI qualification found two interpolation-only presentation hazards: a
guest1280x720 crop was applied directly to a native1920x1080 generated image,
and authority fallback captured the guest image instead of the displayed native
sibling. This produces alternating zoomed world frames and black silhouettes;
it is not evidence of a presentation-order reversal. Corrected real and synthetic
presentation share endpoint-scaled native crops, while replay/fallback admission
checks image provenance and raw encoded format compatibility (mutable UNORM/SRGB
views share RGB bytes, not channel order). Keep `TOTK_NATIVE_UI=0` for interpolation
until this correction has passed Odin video qualification as well as host pixels.
Hybrid never toggles native UI on transitions; UI1 must be qualified explicitly.

Require startup `[hybrid] enabled: native60 -> real30+interp ...`, then in a
sustained heavy world `[hybrid] mode -> real30+interp: sustained low real rate`,
HLE/presenter effective interval2, pair warmup and nonzero midpoint handoffs.
After sustained headroom require `[hybrid] mode -> native60: sustained controller
headroom` and `[hybrid-present] native return: retiring retained real ...`.
The game controller's own state need not be rewritten to interval2; its original
scores remain visible in `[fps-policy] ... controller pre-acquire/decision`.
The1.4.3 controller's stored minimum/maximum may remain2/3 while the FPS60
decision path uses current/candidate1/1. Hybrid validates the live frozen flag,
not an assumed minimum1. The production NVN adapter regression replays that2/3
state at sustained50fps through scene settling with unavailable GPU work:
drop still engages, recovery stays blocked until completed headroom is valid.

Every300 real frames `[hybrid]` reports mode, lifetime time in both modes, switches
and raw-window rate. `[framegen-display]` separates ready/recorded/accepted/
handed-off/expired/no-buffer, and `[hybrid-display]` reports combined real+synthetic
framework present-fence interval bins and missing results for that reporting
window. These asynchronous AHB observations are **not hardware scanout**; copy
fallback has no framework-fence histogram, and zero bins are not proof of60.
Run several-minute sustained60, heavy40–49, threshold oscillation and spike
sections; verify rare switches,33.3ms latency only in interpolation, warmup,
camera cuts, loading/movie/background/resume/window recreation and unchanged
fail-closed authority/budget behavior. High-speed camera evidence is still needed
to claim steady60 on the panel.

## How it works

1. **Extraction.** While the walker replays a frame we track which render passes produce the scene depth and colour
   and which pass composes the final 1280x720 canvas with the UI, and require a proven dependency chain from the
   scene to the canvas (`vk_draw.c` `fg_extract_*`, `vk_framegen.c`). Anything unproven refuses the frame.
2. **Camera.** The scene camera is read from the vertex shaders' camera uniform block (standard layout, plus three
   audited alternate layouts that must match a live root's matrices exactly).
3. **Depth authority.** Reprojection is only as good as the depth it uses. A pass that writes depth from its
   fragment shader, or with a camera we cannot prove, is *unproven*. Per unproven span we copy the affected depth
   rectangle before and after; pixels it changed are *unknown*, everything written by proven passes stays proven
   (commit `3519592`). Unknown pixels are dilated conservatively (4x4 tiles, motion-bound radius) and never
   warped: those pixels show the earlier real frame. Above 35 % unknown the midpoint is not shown.
4. **Generation.** Bidirectional reprojection between the two real frames' depth plus the UI, at the real frame's
   resolution, then handed to the presenter between the two real presents.
5. **Budget.** Generation runs only while real-frame GPU time plus generation fits 30 ms of the 33.3 ms tick
   (p90 estimates, measured probes, real present-fence feedback); heavy frames are never delayed for it.
6. **Presenter.** Six bounded output entries without extra lead; locked lead1/2 uses seven/eight.
   Opt-in FSR scene scaling adds one phase buffer (seven/eight/nine at lead0/1/2), not a new source
   history or permission to reclaim compositor-owned storage. SGSR/off and ordinary three/four-slot
   presentation are unchanged. A midpoint expires only against its own vsync. See the scene-scaling
   ownership and memory-cost explanation in `games/totk/android/APP.md`.

## Safety rules that stay

- Fail closed: unproven structure, unknown resources, camera mismatch or a failed copy refuse the frame; the real
  frame is shown as without interpolation.
- No shader is admitted by identity alone: each admitted depth writer has an audited proof (sentinel-only depth,
  world-plane depth with exact runtime gates). The far-terrain pass with its clip-shrink active cannot be bounded
  exactly and stays unknown; `TOTK_FRAMEGEN_UNCERTIFIED_PLANE=1` exists only as a default-off experiment.
- `TOTK_ACQUIRE_SLACK=2` remains incompatible with interpolation: the acquisition policy refuses the two-frame
  reserve when `TOTK_FRAMEGEN=interp` (or `=1`) is requested. Integration does not enable that combination.

## round14-all integration

`integ/round14-all` combines this feature with round14 and the nxnative CPU-copy ports. The presenter keeps six
entries only for interpolation; ordinary presentation stays at three, or four for the existing explicit override
and two-frame acquisition reserve. Real-frame timeline handoff/slot/readiness observations remain attached to the
retained real picture; synthetic children own no game-frame observation. Compute dependency extraction uses the
ordered interpolation path while retaining round14's translation-statistics instrumentation.

Frame interpolation remains opt-in and its fail-closed authority, GPU budget and late-midpoint rules are unchanged.

## Device results (Thor, Snapdragon 8 Gen 2, field route)

| Build | Gameplay midpoints shown per 300 real frames | Notes |
|---|---|---|
| `22ab7bc` | 0 | canvas tracking not enabled for interp |
| `7639314` | 0 | ~46 % "unknown": proven geometry after the first unproven span was miscounted |
| `3519592` (`apk-interp13`) | up to 287; 230/206 at F3000-3300; 3-32 in heavy stretches | unknown 2.4 % raw (9.2 % dilated); generation ~1.0 ms GPU; heavy stretches refused by the GPU budget (real frames ~28-29 ms p90) |

Menus/title: ~200 midpoints per 300 shown.

## Open issues

- **Walker cost:** extraction and authority tracking add ~2.5 ms of walker time (25.3 vs 22.8 ms), which makes
  more real frames late; displayed hitches rose (132 one-vsync hitches vs 80 in a comparable run without
  interpolation). Candidate fixes: cheaper tracking, or combine with the two-frame run-ahead reserve
  (`perf/runahead2`, `TOTK_ACQUIRE_SLACK=2`), which today refuses to run with interpolation.
- **GPU headroom on the Thor** limits heavy scenes: the 30 ms ceiling leaves no room when the real frame alone
  takes ~29 ms. Generation itself is already ~1 ms.
- **Visual verification:** a recorded pan at the standing test spot engaged too rarely (2-6 per 300, screen
  recording also uses GPU) to judge image quality; a lighter scene or an on-device frame dump is needed.
- **Odin:** earlier builds ran there with the same structure; repeat with `3519592`.

Detailed per-commit analysis, fixtures and logs: `.scratch/interp/report.md` (workspace scratch, not tracked).
