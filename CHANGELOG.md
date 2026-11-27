# Changelog

All notable changes to TotK Native for Android (`com.aure.totk`). The app follows [semantic versioning](https://semver.org);
the game version it runs is shown separately. versionCode = major × 10000 + minor × 100 + patch.

## Unreleased

### Developer tools
- Fix the pre-existing thermal-step veneer on the real 1.4.3 Android rewrite: its mutable floor
  is about 25 MiB past native text, outside an LDR-literal's reach. Use ADRP+LDR and a flag-free
  clamp for the published one/two-tick floor, retaining the same 108-byte tail allocation.
  Model the full rewritten image in host fixtures and verify all floor addresses/register semantics.
- Make menu/thermal, FPS60 hardening/hysteresis and DRS startup guard failures refuse-and-log
  without exiting or partially installing a group. Preflight full module bounds and generated B/ADRP
  references; cover all switch combinations/install orders on the complete Android rewrite geometry,
  corrupted sites/padding and invalid layouts, plus real 30/60-preset startup with player-supplied data.
- Enable PR #44's Android 1.4.3 quick-menu capture fix by default: unset or exact `TOTK_MENU_CAPTURE_FIX=1`
  uses the contributor's original armed-OR-requested admission; `=0` leaves guest code untouched.
  Other explicit values remain off. Keep other writable hosts opt-in, and explicitly exclude both the
  iOS runtime and writable-host iOS export before any write; non-1.4.3 paths are unchanged.
  Our added visibility guard suppressed the fix:
  Odin requests were pending while hidden; original admission removed the black backgrounds in the finite
  30/60-fps comparison, while the guarded version did not. Remove the guarded alternative entirely.
  Retain all-before-any-write bounds, original-word, padding and branch checks. Split the veneer across
  two existing text-tail gaps so thermal-step and FPS60 hardening coexist without moving their code.
  Test real ARM admission/continuation/completion, unset/explicit-on byte equality, explicit-off
  no-write behavior and all six install orders. This does not prove preparation/publication safety
  or a once-per-opening capture bound.
- Include actual menu admission (applied/on, inactive/off, refused/UNKNOWN) in the one-time startup
  switch marker and benchmark comparison/ledger workload signatures. Reconcile raw current-launch
  startup evidence, retain legacy marker parsing without inventing menu OFF, and require resolver
  evidence version 4 for the combined current-launch/menu contract. Pin all three historical opt-outs;
  keep immutable baseline metrics/hashes unchanged.
- Log the frozen effective write-back-alias state and map-warning mode once at first presenter startup,
  including explicit opt-outs, so benchmark tools need not infer build-dependent defaults from menu settings.
- Record effective alias/warning states from complete startup evidence, including the warning mode, in benchmark
  signatures and comparison admission. UNKNOWN never matches; missing lines are not silently off. Preserve pinned
  pre-switch baseline numbers/hashes and require explicit `TOTK_WB_ALIAS=0 TOTK_MAP_WARN=0` for historical controls.
- Reconcile every positioned benchmark switch-state record inside an independently closed startup bracket.
  Reject stale, duplicate, malformed and conflicting records per switch before marker or pre-switch-provenance
  admission; require current evidence metadata so older resolver results cannot bypass UNKNOWN rejection.
  Preserve window metrics and immutable baseline identities.
- Scope all three effective-switch states, including menu admission and both startup-bracket ends,
  to the current launch in retained ten-launch logs. Historical markers or a migrated pre-history segment
  never authenticate missing, different or incomplete current menu admission.
- Make optional frame-timeline and fence-evidence ring producers single-attempt/nonblocking, with counted
  dropped updates and skipped snapshots. Reuse per-walk elapsed clocks instead of enabling recorder
  opcode/category probes, and batch recorder-worker draws/descriptors at slot assembly instead of each job.
  Retain slot/walk/input/presentation evidence and ordinary default-off behavior.
  Gate the new per-walk/per-assembly accounting bodies before all counter/slot work when disabled;
  test both probe regions stay unentered with timeline off, including with explicit recorder profiling on.
  Measured AYN Thor, 30-fps standing, 20-minute launches: walk CPU about 15.7 ms/frame off,
  20.4 ms old-on (+29.6%), and 15.5 / 15.7 ms new-on (−1.3% / +0.3%, within noise).
  New-off was −0.4% / −0.8% versus the measured main baseline, within noise; sample loss ≤0.07%
  of producer-update attempts. Observed intervals ≥60 / ≥80 ms per 20 minutes fell from 20 / 11
  old-on to 1 / 0 and 4 / 0 new-on. The first run's ordinary >50 ms gate failed (new-on 37 / 63,
  off 19–37 per 20 minutes), but an eight-launch paired follow-up CLEARED the rewritten diagnostic
  on this AYN Thor, one Hateno scene, standing only, 30-fps cap: ON 31.43 / 10.10 / 22.44 / 30.30
  (mean 23.57) versus OFF 21.32 / 39.27 / 33.67 / 35.92 (mean 32.55) per 20 minutes;
  paired ON-minus-OFF +10.10 / −29.17 / −11.23 / −5.62 and walk-CPU overhead +0.19%.
  Frozen rule: mean difference ≤10, ON mean ≤1.30×OFF mean, not all pair differences positive,
  at most one ON value above OFF maximum; all passed. The first run's single high launch is
  consistent with launch-to-launch variation, not a demonstrated diagnostic effect.
  It still adds per-frame work when on and remains a diagnostic: prefer OFF for final tail/performance
  acceptance unless present identically in both arms. Old-on worst-frame figures are suspect and
  walk CPU about 30% inflated. CPU uses medians of ordinary block means; >50 ms rates use
  complete-report exposure estimates, unlike exact 20-minute timeline windows. Fixed-order,
  single-device/scene standing evidence does not establish general tail safety. Measurements are
  old/main 8bbbce5 versus rewritten 8dc331f, not the integrated head; see DEBUGGING.md and
  `.scratch/perf/frame-timeline-fix/thor-pairs/report.md` for the follow-up source and limitations.
- Add enabled-only `TOTK_WB_ALIAS` path coverage (unset or `=1`): cumulative operations/bytes by landing, report/event store
  and publication, separated into alias and fallback reasons at periodic memory reports, Android pause and normal exit.
  Publish pause-report enablement atomically so early Android lifecycle callbacks can race Vulkan initialization safely.
- Add launch-only, default-off `TOTK_BENCH_TIME=HH:MM` for restored-save benchmark arms.
  It pins the native effective clock, overriding timeline/sleep/day progression; weather and randomness remain uncontrolled.
  Host guards and ARM64 state fixtures do not establish device HUD/sky or unchanged-save behavior.
- Add default-off `TOTK_OVERLAY_SYS=1` full-overlay rows for Android thermal status/headroom, the main game's
  sampled CPU policy current/hardware-max clock ratio and cap marker, available/total RAM and game RSS.
  The existing background sampler owns all reads (1 Hz; headroom at 10 s); absent fields never move the layout.
- Release benchmark camera pans at the timed endpoint before snapshot reads, using one symmetric device-side batch.
  Interrupt/failed-command cleanup cancels that owned batch promptly instead of queuing behind its busy root broker.
  The fake-device cancellation regression measures the completed release within 6 s independently of a
  load-tolerant 30 s host-teardown timeout.
  Add an explicit restart-quartet O/B/B/O comparison rule with paired signs and reference-spread admission, without
  changing in-session direction confirmation. Report window compile provenance and unavailable scene state;
  optional bounded graphics-calm warm-up is off by default and never filters compiling measured windows.
- Add opt-in rooted-device CPU profiling with NDK simpleperf frame-pointer stacks,
  exact-build symbol checks, shell-accessible staging, and verified perf-control restoration.
  Per-thread period-weighted self/inclusive reports separate walker, recorder, submit and game CPU.
- Give newly linked `libtotk.so` a SHA-1 GNU build ID retained in the packaged library,
  so native profiles can identify matching unstripped symbols without relying on source revision alone.
- Add `TOTK_FRAME_RATE_HINT_HZ`: request integer rates advertised by the current game-window display,
  or `max` for its highest rate (including fractional Hz), through FIXED_SOURCE window/AHB-child hints.
  Ordinary fixed-rate Samsung gameplay now defaults to `max`; elsewhere explicit enablement is required.
  Invalid, unavailable or zero rates fall back to content-rate hints; Android's user limits remain authoritative.

### Runtime
- Correct ordinary Android capped pacing from guest 60-Hz ticks to display slots: 30/60-fps caps
  remain 30/60 on 60/120-Hz panels. Non-divisor displays round up to whole slots (at 144 Hz,
  30/60 choices yield 28.8/48 fps). `TOTK_FRAME_RATE_HINT=0` disables refresh votes, not this cap fix.
  Explicit display/acquire clocks, frame generation, FPS Auto and uncapped policies retain their own authority.
- Keep validated refresh preferences while foregrounded, clearing on pause and restoring on resume.
  The Samsung controller-only idle fix still requires affected-Samsung testing; local handhelds cannot verify it.
- Show non-touchable startup stage/progress until the first real compositor handoff, not measured scanout.
  Reuse metadata-bound verified RomFS overlay proofs on warm launches; this is not a fresh full hash.
  `TOTK_ROMFS_VERIFY=full` forces rehashing.
- Retain the current native log plus up to nine old launches in the selected game-data folder, with lock/temp
  siblings. Old launches retain roughly 512 KiB each (head/tail; middles discarded); the live log is unbounded.
  Failed compaction preserves evidence and may exceed that history budget. Benchmark tools select the latest launch.
- Stop startup-popup polling and dismiss the popup when its activity finishes or is destroyed;
  repeated resume calls keep a single poll queued. Host tests execute the production Java UI
  with framework doubles and the native progress snapshot through a real host JVM/JNI bridge.
  Resolve JNI headers from the selected compiler's actual JVM home, including version-manager shims;
  missing headers skip with a clear prerequisite reason.
- Dismiss the startup indicator before any game-activity update or previous-session notice can take focus.
  A pending update comes first, then the notice; both wait for the first real handoff. Keep session-token
  recording independent of that UI gate and preserve background hold and refresh-request lifecycle.
- Report companion tile, overview, decoration and image LRU occupancy/budgets, allocation-byte high water,
  lookup/decode counts and evictions through the native log. Snapshot/fill/eviction delivery shares one process-wide
  ten-second completion-based limit across recreated stores. A second ten-second allowance batches lifecycle events;
  at most two lines per rolling ten seconds, only while visible, with monotonic store IDs and bounded/loss-counted
  create/close history. Counters remain per store; cache budgets, decode policy and eviction order are unchanged.
  Snapshot into preallocated storage, then format/write on one bounded process-wide background task outside cache
  locks; slow native logging cannot occupy the draw or bitmap-decoder thread. Logcat mirroring remains opt-in.
- Follow Link's environment-based map layer, display in-game map coordinates without losing integer-boundary
  precision, and separate overview/detail caches to stop stationary tile eviction loops. Reject invalid, unreadable
  or replaced layer samples using the previous layer heuristic; always keep the visible detail tiles plus one, scale
  only the extra cache margin by Android memory class, and serialize resize decisions. Use plain layer parameters
  even when their flag bits are clear: the tested Thor surface scene had `01,01,00` and the plain values gave the
  correct layer; scenes where overrides differ remain untested. Log fallback reason classes at most once per minute.
  Thanks to @Dorkington ([#47](https://github.com/AurelioB/totk-android/pull/47)).
  Host regressions cover native layer guards, native JSON → Java coordinates and deterministic cache convergence;
  surface/sky/Depths transitions, zoom appearance, memory and decoder CPU still need dual-screen device checks.
- Enable `TOTK_MAP_WARN` mode `1` by default when unset; `=0` disables it and starts no sampler for this feature.
  Start lazily in the game presenter, not on library load or in `:setup`; log one startup line.
  Adapt this warning's own scans to 60/20/5 s below 40% / 60% / above, with 37% / 55% de-escalation
  hysteresis, a 15,000-row observed-load headroom reserve, >2,048-row jump hold and worker-only five-second
  probes of the existing pool-lifetime epoch. Reuse fresh samples unless a later pool change invalidated them.
  Give the smaller-limit headroom guard separate hysteresis (release strictly below entry minus 3%
  of the current sampled limit), avoiding 20/5 s toggling at 13,835/13,836 rows on a 32,768 limit.
  No new game/render/fault-path work is added. Mode `2` logs cadence, activity, jump and requested/reused
  scan counts; TEST-ONLY forced-band controls support safe checks. Mode `3` retains its five-second test.
  This reduces warning overhead, **not mapping growth**. The warning remains **best-effort**: a jump is
  only detected when sampled, hints miss pool reuse/driver growth, 15,000 rows is not an upper bound,
  and repeated loads can cross 88% inside a nominal 60/20/5 s unsampled interval plus scan/scheduling delay.
  It can be late when growth exceeds the stated headroom assumptions; no admitted tail improvement is claimed.
  Log at 80%; show `MAP LIMIT NEAR` /
  `SAVE AND RESTART GAME SOON` at 88%, even with the performance overlay off; clear below 84%.
  Hidden kernel limits use 65,530; unreadable counts clear the notice and log once. Startup-only
  percentage overrides support safe low-threshold testing. Add diagnostic `TOTK_MAP_WARN=2`
  per-scan monotonic start/duration/rows and ordinary-frame clock origin; mode `1` adds no per-scan logging.
  TEST-ONLY `=3` changes thresholds after 12 and 24 warning samples to show, hold and clear the
  actual notice without adding mappings or restarting. It is not an ordinary-play warning mode.
  Initial Thor checks observed the notice/default-off state; later TEST-mode checks showed hold/clear after
  threshold changes, not natural recovery through 84%. Thor/Duo scan-cost observations used inherited scheduling.
  Normalize warning-only workers to absolute nice >=10 and launch-cpuset spare/background CPUs; log actual
  priority/affinity and placement errors. Preserve lag-created scheduling and its existing scan period rather than
  claiming a new scheduler freshness bound. Re-check scheduling, scan durations and long-frame coincidence on
  the final APK on lag-off Thor/Duo and lag-on Odin before qualifying this change.
  Final device checks are required before landing the default-on change. This warns only: it does not prevent the map-limit abort,
  fix map growth or change mapping/protection/admission policy. Diagnostic modes `2` and `3` remain opt-in.
- Correct fragment inputs that their vertex shader never writes, targeting wind/air-flow white boxes
  ([#4](https://github.com/AurelioB/totk-android/issues/4)). Thanks to @Dorkington
  ([#43](https://github.com/AurelioB/totk-android/pull/43)); keep the credited MIT adaptation.
  The varying correction is on by default (`TOTK_VK_VARYING_FIX=0` opts out); integer folding and all
  other module postprocessing still apply. New cache namespaces seed unchanged driver entries from a
  context-matched predecessor without reusing its prewarm completion bits or changing shader packs.
  Startup cache-only probes publish hits before play; only misses queue for compilation. Rollback files
  remain untouched. Worker seed-IO accounting is thread-safe and session logs count patched/fallback
  shader pairs. The contributor's independent NaN min/max/clamp rewrite is retained as
  **default-off** `TOTK_VK_NAN_MINMAX=1`, pending separate device qualification. Host checks cannot establish
  the wind picture, ordinary-scene regressions, prewarm time or frame-time effects on devices.
  Keep presentation-time varying statistics independent of module creation. Cache-only probes now defer
  absent/in-flight replacements without IO or a shader-module wait; unrelated interfaces build concurrently.
  Mandatory synchronous builds can still wait for required driver work; no device latency bound is claimed.
- Retire cached and deferred texel-buffer views of an owning pool's exact Vulkan buffer before unbacking destroys it,
  using the existing arena-buffer release mechanism and existing GPU-use wait. Image/storage invalidation is unchanged.
  Restore guest pages/private aliases under the pool lock before view retirement, so concurrent same-range
  registration cannot have its new backing overwritten by the old pool's restoration.
- Pack multi-element direct and frame-generation descriptor writes into fixed Vulkan-typed scratch arrays.
  Singleton direct writes retain the existing batched path; descriptor hashing and immutable ownership are unchanged.
- Retry transient image creation/allocation OOM failures after 1–64 submissions, with at most four retry attempts
  per submission across recorder threads. Permanent unsupported-format/type failures remain failed; retry logs are bounded.
- Refresh the shared shader list with seven more upstream variants applicable to game 1.4.3
  (`5fb080496ab7`, V rows only). Fresh host translation prepares 25,039 modules instead of 25,032,
  with zero failures and about 430 KiB of additional packed data. Existing shader packs stay valid;
  run a full shader rebuild to receive the added coverage.
- Enable launch-only `TOTK_WB_ALIAS` by default when unset (`=1` also enables; `=0` restores the original path):
  use a retained same-backing private CPU mapping
  for supported dirty-tracked write-back extents and eligible report/event/uniform stores, without temporarily
  making protected guest pages writable. Preserve CPU-write detection, GPU synchronization and zero-on-recommit;
  unsupported backing and contended report/event/uniform publications keep the old path. This closes the
  protected-page temporary-RW race, not already-writable/unwatched aliases. Cost is extra mappings/VA, not copied backing.
  Off preserves the original path and logs. First AYN Thor device evidence, over five common completed loads:
  texture GPU-unprotect boundaries 0 versus 17,214; whole-process row growth averaged +1,025 versus +4,535/load.
  No switch-attributable picture corruption was observed. Pooled standing frame tails were SAME, but some individual
  phases had worse tails; this one ordered alias/legacy pair is not a speed proof. Odin and Duo were untested
  in that initial campaign. Later Duo combined no-env/opt-out checks passed; Odin's restart quartet remains
  INCONCLUSIVE because workload admission failed, not accepted as a non-regression or waived qualification.
  This mitigates this fragmentation source, not all mapping growth. Final device checks are required before landing
  the default-on change; existing unsupported-backing/registry-contention fallbacks and their diagnostics are unchanged.
- Correct the experimental private writer's virtual-mapping authority across production MapVirtual replacements,
  including anonymous unmaps and fallback remaps; serialize replacement and invalidation with borrowed writers.
  Watch-slot-exhausted synchronous records keep the original no-token guest-store path rather than bypass ownership.
- Diagnose interrupted Android game sessions on launcher/title return using matched platform exit history,
  with a once-only, controller-dismissable recovery notice. Low-memory exits explain possible unsaved
  progress loss; unknown endings remain unclassified. Updates, normal closes and setup-only exits do not
  show a game-loss warning. Default on; exact `TOTK_EXIT_NOTICE=0` silences it. No survival guarantee.
  Receipt/diagnostic IO and cross-process delivery claiming run off the UI thread; native lifecycle only enqueues.
  Platform summary tokens and exact exit-record identities replace wall-clock ordering. Missing/rotated history
  or cleared state stays silent; unshown delivery survives activity recreation without duplicate dialogs.
- Add experimental `TOTK_GAME_HOLD=1`, off by default: our visible Settings menu binds the live game at foreground-client
  importance and defers its pause-forced pipeline-cache snapshot. Real background and save-safe Close still flush;
  a kill during deferral can lose new cache entries. The save protocol is unchanged, not kill-proof. Binding lifecycle
  and the game's pause/1 s/5 s `oom_score_adj` are logged. Host coverage is not Duo survival evidence.
- Fix shader translation for compute texture gathers with runtime offsets.
  The translator now lowers those offsets before emitting SPIR-V instead of putting runtime values in constants.
  Existing generated shader seeds need replacement to receive the corrected modules.
  Replace seeds while the game is stopped; stale loose shader files must not override the corrected pack.
- Enable the companion's opaque cover by default on devices with a built-in second screen;
  `TOTK_COMPANION_COVER=0` turns it off. A companion window alone does not make Android stop the app
  underneath it; an opaque activity does. Tested only with the Cocoon launcher: it was stopped in every
  cover-on launch on both the Retroid Pocket Duo and AYN Thor.
  On Duo, launches started from the launcher pooled 28.70 fps and 101 frames over 50 ms per 24 windows
  without the cover, versus 29.22 fps and 40 with it. The frame-rate difference is inside the comparison
  tool's tolerance; the tool rates the pooled result better.
  On Thor, launcher CPU use during gameplay fell from about 27% of one core to about 0.05%, and compositor
  CPU use from about 19% to about 17%, with no frame-rate change (44.80 → 45.32 fps standing;
  44.65 → 44.69 fps panning).
- Add experimental `TOTK_VK_REC_ONE_PRIMARY=1`, off by default: record each submitted chunk into one main
  command buffer instead of one per render pass. On the AYN Thor, measured GPU time per frame was about 5% lower:
  18.67 → 17.65 ms standing and 18.64 → 17.63 ms while panning. Frame rate was about 2% higher
  (45.20 → 46.04 fps standing; 44.45 → 45.34 fps panning), which is inside the comparison tool's tolerance,
  so it is not counted as confirmed. The AYN Odin 3 showed no frame-rate change;
  adoption remains undecided on the Retroid Pocket Duo.
- Fill screens larger than the docked 1920×1080 canvas instead of showing it 1:1 inside black bars (e.g. a
  2400×1504 tablet used 57% of the panel, now 90%). The front buffer keeps the canvas size; the compositor
  destination (or the copy fallback's blit) is aspect-fitted to the window. Panels no larger than the canvas,
  such as the Odin's 1920×1080, are unchanged. Thanks to @kestrelshader42 ([#21](https://github.com/AurelioB/totk-android/pull/21)).
- Keep `totk.log` as the direct primary log, but make its duplicate logcat mirror opt-in (`TOTK_LOGCAT=1`).
  Normal launches no longer start its tail thread or wake logd for every frame log. Crash reports remain enabled.
- Let the live-control reader sleep when `totk.ctl` is absent, including after unrelated files are created or deleted.
  Keep its directory watch until the control file appears instead of repeatedly waking on its own watch removals.
- Keep the stall watchdog enabled, but reuse the present timestamp/frame instead of reading a second clock and
  incrementing an atomic counter every frame; publish immutable stage names only when they change.
- Add default-off `TOTK_VK_GPU_PROFILE_PIPELINES=1..16`: bounded rotating GPU timestamp brackets for
  contiguous actual-pipeline/full-shader-variant groups, including recorder secondaries and separate compute.
  Schema-3 rows retain draw/primitive knownness and sampling/loss authority; the host analyzer ranks exact
  pipelines and shader pairs without treating a pass's first shader or draw count as their cost.
  Existing pass profiling stays unchanged when this switch is off; on-device overhead still needs calibration.
- Make enabled pipeline profiling default to bounded sampled-key 300-frame compact JSONL windows,
  suppressing per-group/per-frame rows while preserving parent pass/window aggregates. Explicit
  `TOTK_VK_GPU_PROFILE_PIPELINES_RAW=1` restores the large raw stream; profiling with PIPELINES off is unchanged.
  Intern exact pipeline/full-shader identities with bounded inactive-key eviction and per-file dictionary
  authority. Preserve actual-denominator moments, completed-frame domain eligibility, population exposure
  and explicit loss/overflow; flush partial windows once at close without inventing GPU retirement.
  Publish worker CPU, cumulative output bytes, backlog/memory bounds and a deterministic no-GPU
  4096-key churn fixture using the production queue/aggregation/output path. Host capacity is not device proof.
- Amortize compact pipeline-window encoding across completed frames using one fixed report snapshot,
  rather than holding query-lease service for the report-edge formatting burst. Close drains the snapshot;
  headers include its bounded memory. Count and retire activated pipeline timestamps in the current
  shutdown authority, retaining unavailable endpoints as outstanding. Profiling remains default-off.
  Limit qualified pipeline captures to ≤30 fps (Duo or capped Thor); 45-fps/uncapped captures remain
  unqualified and must fail closed on footer drops/missing coverage. The fixed lease pool is not enlarged.
- Refresh the shared shader list with the 301 upstream variant rows that apply to game 1.4.3
  (upstream list commit `a8ff9bc429bf`, V rows only): a full shader rebuild now prepares 25,032 variants
  instead of 24,783 and the shader pack grows by about 29 MiB. Existing shader packs stay valid; the added
  variants arrive when the user runs the full shader rebuild. Upstream's new compressed pipeline-row and
  recipe-row formats are not imported.
- Enable exact integer shader-address folding by default (`TOTK_VK_INT_FOLD=0` turns it off).
  Earlier tests reduced total native shader instructions by 6.58%; capped GPU-work estimates fell about
  2.3% on Duo and 4–5% on Odin (not confirmed battery savings), while Thor throughput stayed the same.
  Odin's uncapped standing sample rose from
  61.93 to 63.81 fps; these are workload-specific results, not a general frame-rate promise.
  With this port: pictures were correct on the Odin (six scenes) and the Duo (15 minutes). On the Duo, uncapped,
  frame rate was unchanged (31.81 → 31.85 fps standing, 30.67 → 30.52 panning) and GPU time per frame fell from
  24.61 to 23.93 ms and from 25.08 to 24.47 ms; no measured value was worse, but the comparison counts as
  inconclusive because the GPU clock differed by 3.3% between the two settings. On the Thor, on against off:
  45.44 → 48.24 fps standing and 45.03 → 46.84 fps panning, GPU time 18.57 → 18.00 ms and 18.53 → 17.94 ms
  (both rated better); the slowest 1% of frames changed by -1.3 / +1.4 ms, inside this device's
  launch-to-launch spread. On the Odin: 75.68 → 75.88 fps standing and 71.65 → 71.99 fps panning (same),
  GPU time 9.36 → 8.87 ms and 9.46 → 9.00 ms, nothing worse. The driver's pipeline cache is rebuilt once in
  the background after this change (about 5 minutes on the Odin, about 17 minutes on the Thor, much longer on
  the Duo while playing); the shipped shader set stays valid.

### Menu
- Add **Unlimited** to Frame rate: variable real-game updates up to the existing 120 fps safety ceiling, with
  the realtime game clock unchanged, frame generation and frame lag off, and the scripted-camera 30 fps floor intact.
  Request the window display's highest advertised refresh rate; Android may keep a lower mode. This choice is best
  with a high-refresh screen and may run hotter and use more battery. Existing 30/40/60 choices and every preset
  retain their exact settings; leaving Unlimited removes its timing and refresh request.
- Enable protected frame lag only in the Snapdragon 8 Elite-class (Odin3, `CQ8725S` / `SM8750`) native60 fps
  menu profile, with frame generation off, including first run and existing stores. Developer `totk.env`
  `TOTK_FRAME_LAG=0` still wins; runtime-unset, other chips and non-60 settings remain unchanged.
  Retain automatic600-draw chunking when lag resolves on and native VMA pressure is absent.
  Capped Odin tests improved static/pan throughput and33 ms counts; a descriptive pan p99 tail worsened,
  so this is not a tail or responsiveness guarantee. Duo/Thor do not gain the policy.
- Add **40 fps** to Frame rate without changing existing presets or 30/60 settings. It caps real game updates
  at 40 with normal game speed and frame lag off, requesting 120 Hz when advertised. Presentation slots follow
  the actual display clock: smoothest on 120 Hz; on 60 Hz (including a user-imposed cap) it is uneven.
  Selecting 40 disables frame generation; returning to 30/60 removes the 40-specific timing and refresh request.
- Preserve the scripted-camera 30 fps safety floor at 40 fps using a 33.333 ms producer period and matching
  two-/four-refresh slots on 60/120 Hz. If 120 Hz is unavailable, request at least 60 Hz content cadence so a
  30/60 Hz panel does not drop to 30 Hz; ADPF work targets and the existing 30/60 settings stay unchanged.
  The 120 Hz three-refresh path is fixture-tested only, not yet device-qualified; device smokes remain at the user's 60 Hz limit.
- **Keep the GPU busy** in Graphics → Performance feeds smaller batches earlier and requests clocks that keep up.
  On by default only on Snapdragon 8 Elite-class chips (Odin 3), independently of the chosen preset; it uses a
  little more GPU and CPU clock, power and heat. Restart-based Odin 3 tests raised Hateno from 46.5 to 57.3 fps
  and Lookout from 52.2 to 57.1 fps (4/4 pairs); Rotsumamu stayed at the 60 fps cap. At the 60 fps preset the
  Duo's p95 worsened by 4–5 ms (4/4 pairs), while Thor was neutral, so both keep it off.
  Upgrades without a stored choice adopt the chip default; explicit choices survive. Presets no longer change
  or match this option. Thor and Duo defaults retain 1.0.1's settings.env bytes for every preset, and frame
  generation leaves the option inactive.
- Make controller hysteresis part of the Odin / Snapdragon 8 Elite-class native 60-fps menu profile:
  `TOTK_FPS60_HYSTERESIS=1` is generated only for `CQ8725S`/`SM8750` at 60 fps without frame generation,
  including existing stores and custom mixes, independently of GPU busy. No extra visible option or stored
  preference is added. Runtime unset remains native/off; developer `totk.env` `=0` overrides the menu.
  Other chips and every non-60 preset keep their previous settings.env bytes. Device validation is required
  before this branch is merged.

## 1.0.1 (versionCode 10001), game 1.4.3

Fixes for the first reports, plus a few improvements.

- Updates: when the game starts, the app checks GitHub for a newer version (one tiny request, only when online, never delays the game) and offers to download and install it; a setup step and a menu switch turn it off.
- Update safety: other apps cannot authorize an update through the exported menu; downloads stop if they stall or exceed ten minutes, and the exact bytes handed to Android are checked again before installation.

### Game
- The game pauses, with its sound, while it's in the background or the recent-apps screen is open (on dual-screen
  devices, using the other screen doesn't pause it).

### Setup
- The container route (XCI/NSP + update + `prod.keys`) no longer fails with "Could not create romfs.fifo" when the
  game data goes to the SD card or to a folder you picked. The merge pipe now always lives in the app's private
  storage; an interrupted merge's leftover pipe is removed on the next run.
- Setup errors name the call that failed and its real reason (1.0.0 could report "No such file or directory" for a
  different error).
- Time estimates match measured runs: about 2–3 minutes from decrypted files, about 15 minutes from containers, plus the
  copy time to an SD card.
- Setup stopped: a **Share details** button sends the error and the setup logs (no game files or keys) through any
  app, for bug reports.
- Setup skips the second-screen art on devices with one screen; on dual-screen devices that don't have it yet, Start
  game offers to prepare it.

### Menu
- The menu no longer reports "The game is running" when it isn't: its own background checks are no longer mistaken
  for the game.
- When the game really is still running in the background, the affected actions say so, and a **Close the game**
  button closes it safely (waiting for an in-progress save) and then carries on with what you chose. This also works
  after Android freezes the background game: a temporary menu binding wakes it without bringing its task forward.
- Graphics driver: Turnip drivers can be picked as the .zip they're distributed in (AdrenoTools format), not only as
  a .so.

### Second screen
- On dual-screen devices the game always opens on the main screen, with the companion on the second one, even when started from the second screen.
- The controller keeps working after you touch the companion or an overlay on the second screen (such as AYN's
  performance overlay): Android sends the buttons to the last-touched screen, so the game now takes them back.
- Fused materials use the game's own names (for example "Armor Shard" instead of "Obj Armor Spike B"), including world
  objects that are not pouch items and the Russian names. Existing second-screen art updates automatically on the next
  start; unknown objects fall back to readable text.

## 1.0.0 (versionCode 10000), game 1.4.3

The first public release: one generic, signed APK for arm64 Android 13+ devices with an Adreno GPU. It contains no
game files; everything game-specific is built on the device from the player's own dump.

### Setup
- On-device setup from either route: decrypted files (the 1.4.3 RomFS image or its split parts, plus the ExeFS), or
  containers (base game XCI/NSP, the 1.4.3 update NSP and `prod.keys`, merged on the device). Every input is checked
  against the pinned 1.4.3 hashes before anything is written; wrong picks are refused with the reason.
- The game code is rewritten for Android on the device, the RomFS is copied and verified, the shader seed is generated
  and the second-screen art is prepared, each with its own progress; setup can be cancelled and resumed.
- Storage choice: the app's private storage (recommended), the SD card's app folder, or a folder of your choice with
  Android's optional *All files access*; the game data can be moved later, with a verified copy before the old one is
  removed. FAT32 cards get the RomFS image in parts automatically.
- Per-device graphics profiles from the exact chip model, each Dragonwing part with its Snapdragon counterpart:
  Snapdragon 8 Elite class (CQ8725S, SM8750) 60 fps; Snapdragon 8 Gen 2 class (QCS8550, QCM8550, SM8550) Balanced;
  Snapdragon 7 Gen 4 class (CQ7790S, CQ7790M, SM7750) Light. Other devices start on Light, marked as untested.
  Frame generation and the other experimental options are off in every profile.

### Menu
- A pre-game menu with Graphics (Performance, Picture and Screen tabs, presets), Controls & audio, Game data, Saves and
  About; controller navigation throughout. **+** starts the game from any screen.
- About shows the app version, the game version, the build commit, the detected chip class and screens, the
  licences of every bundled component, the project's home (github.com/AurelioB/totk-android) and the projects it is
  built on (MacNativeGames by ly05010419 and kitcatkode's Android port of it).

### Saves
- Export to a zip, import from a zip or folder (this app's exports and JKSV, Checkpoint, Ryujinx or yuzu dumps),
  automatic backups before every change, and an optional save folder that is kept in sync after every in-game save.
- Each slot shows its picture and the date and time the game saved it (as on the game's own load screen).

### Second screen
- On devices with a built-in second display (AYN Thor, Retroid Pocket Duo) a companion shows Link's status, the map
  and the inventory, with item actions: equip and unequip, eat and drink, take out materials and Zonai devices, fuse
  materials to arrows. The built-in panel is detected from the displays present, never from the chip; external
  monitors, cast and overlay displays are ignored.
- Every item shows its game name and description, also the ones the game names after another item (caught fish and
  insects, bundles, the opening's Master Sword and Champion's Leathers) instead of the internal actor name. Art made by
  an earlier version of the app is updated on its own when the menu opens; the game stays playable meanwhile.

### Game
- The game runs natively at 30 or 60 fps through the bundled, pinned Mesa Turnip Vulkan driver, with optional render
  resolution, FSR 1 / Snapdragon GSR upscaling, FXAA/SMAA, shadow quality and a performance overlay (**−** then **R**
  while playing).
- The bundled Turnip describes the Adreno 722 (Retroid Pocket Duo) with its two CCUs and two SPs (Mesa had one of
  each), which ends the burst of GPU page faults the Duo logged at every start of the game.
- Experimental and off by default: frame generation (locked, adaptive, LSFG with your own DLL) and the native HUD.
- The game's text prompts (naming a horse) open a dialog with the system keyboard: it starts with the game's text, applies
  the game's length limit and refused characters while you type, and OK / Cancel (or a gamepad's A / B, in the button
  layout chosen in Controls & audio) answer the game.
- Rumble and motion controls: the game's rumble plays on the controller's own motors (or the device's vibration motor),
  and motion aiming reads the controller's gyroscope (or the device's, following the screen's rotation). Controls & audio
  sets the rumble strength (off, 25–100 %) and turns motion controls on or off; a device without a vibrator or gyroscope
  does not offer that feature to the game.
- When the graphics driver loses the GPU (Vulkan device lost), the game still has to close, but the menu then opens
  with "The game stopped" and the reason instead of leaving you on the home screen without a word.
- `TOTK_ASPECT` (developer option, totk.env, e.g. `21:9` or `4:3`) shows the game's view at that ratio with black bars
  instead of stretching it over the screen, and the HUD and menus keep their proportions: in the middle 16:9 for a wider
  ratio, scaled down to the shown width for a narrower one.
- `TOTK_AUTOLOAD=1` in totk.env (or a test session's settings.env) loads the newest save again; the app used to read it
  before those files.

### Tested devices
- AYN Thor (QCS8550, Adreno 740, Android 13), AYN Odin 3 (CQ8725S, Adreno 830, Android 15), Retroid Pocket Duo
  (CQ7790S, Adreno 722, Android 16).
