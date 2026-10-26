# NVN → Vulkan translation: research, options and progress

Living document for making the Android port's command translation cheaper. It records what we measured, what other
translators do, which Vulkan mechanisms our drivers really have, the options that came out of that, and the result of
every option we try. Update the status table and the results log with each experiment; do not delete failed ones.

## 1. The problem, measured

The game's NVN calls are recorded on its own threads (`hle_nvn.c`), replayed at submit by one **walker** thread
(`nvn_vk.c`, `vk_draw.c`) that resolves every draw's resources, and handed to two recorder workers (`vk_record*.c`)
that emit `vkCmd*` into command buffers. The game's main thread waits for the walker, so walker time is on the
critical path.

Thor (Snapdragon 8 Gen 2, Adreno 740), field route, 60 s simpleperf capture (`t-pr-r13`), walker ~24-25 ms/frame,
~3,900 draws, ~150 passes and ~25 dispatches per frame:

| Thread | Our code (libtotk) | Turnip | libc | kernel |
|---|---|---|---|---|
| walker (`totk-walk`) | 74.2 % | 6.3 % | 11.2 % | 6.7 % |
| recorder workers (`totk-recw0/1`) | ~45.5 % | ~37.8 % | ~10.8 % | ~3.8 % |

The walker barely talks to the driver: its time goes into deciding what each draw reads. Inclusive shares (they
nest, do not add): `bind_stage` 28.4 % (~7.1 ms), `record_buffers` 16.3 % (~4.1 ms), `draw_touch` 12.8 % (~3.2 ms),
program/pipeline lookup ~1.6 ms, submit-time uniform snapshot preparation ~0.9 ms. Self time of the four frame-lag
lookups (`lag_binding_key`, `lag_original_pending`, `lag_physical_window`, `find_range` neighbours) is ~5.4 ms.
Frame-pointer unwinding drops many call chains, so inclusive shares are approximate; self shares are exact.

Our caches already hit: uniform metadata 80 %, reusable uniform copies 76 %, vertex 91 %, index 83 %, program
99.7 %. Only 38 % of draws take their whole binding set from the freeze memo (2,949 hits / 4,861 misses per frame).
Each hit still pays a lookup, so another cache is not the answer: **the number of decisions has to fall**.

## 2. What the drivers support (checked in source at the deployed revisions)

Deployed drivers, from the app's startup log: Thor `Mesa 27.0.0-devel (git-adc573c4c7)` (our bundled Turnip),
Odin 3 `(git-f7c93a1352)` (the bundle's base commit, without our 13 patches; give both devices the same
`driver.so` before comparing them). Both revisions advertise, in `src/freedreno/vulkan/tu_device.cc`:

| Mechanism | Turnip | We use it |
|---|---|---|
| Descriptor update templates | yes | yes (3 call sites) |
| Push descriptors | yes, but each push copies the whole set into command-stream memory (`tu_cmd_buffer.cc:5250-5277`) | tried, no gain (VULKAN.md "Push descriptors: tried, not kept") |
| Descriptor buffer, descriptor indexing, mutable descriptors | yes; ~16.7 M sampled images per set (`MAX_SET_SIZE` 1 GiB / 64 B) | no |
| Buffer device address | yes | no |
| Inline uniform blocks | yes, 4 per stage x 256 B | no |
| Extended dynamic state 1/2 / 3, vertex-input dynamic state | yes | 1/2 only |
| Graphics pipeline library | yes | opt-in branch |
| Multi-draw | yes, up to 2,048 per call | no |
| Nested command buffers; render-pass-continue secondaries appended by reference | yes | no |
| Shader objects, device-generated commands, descriptor heap | **no** | - |

Limits: 8 descriptor sets, one reserved when needed (7 usable); 256 B of push constants. This is source-level
capability; the features our app actually gets enabled should be logged at startup before a design depends on them.

Facts about our own design that shape the options:

- NVN GPU addresses are CPU addresses (`nvnMemoryPoolGetBufferAddress` returns the pool's pointer) and pools are
  dma-bufs mapped at the game's addresses (`nvn_vk.c` header), so a guest address converts to a device address as
  `pool BDA + (addr - pool base)`.
- `BindTexturesDeferred` handles must be read at submit (`hle_nvn.c`, the 2026-10-15 flicker), and uniforms are
  snapshotted at submit because the game rewrites them right after submitting. Full translation at record time is
  therefore not possible.
- On Adreno 7xx and later, ir3 turns global loads into fast constant loads (`ldg.k`) only when they are 16-byte
  aligned, non-writable and speculatable (`ir3_nir.c`, `ir3_nir_can_lower_to_ldg_k`). Pointer-based uniforms must
  meet that or they get slower on the GPU.

## 3. What other translators do

All of them make per-draw work cheap the same way: **resolve when state changes, not when a draw happens**.

| Project (licence) | Technique that cuts per-draw CPU | Relevance |
|---|---|---|
| DXVK (zlib) | 64-bit dirty mask plus binding bitmasks; emit only deltas; constant-buffer "discard" renames to a fresh slice that lives until retired | option 1, option 3 |
| VKD3D-Proton (LGPL-2.1+, ideas only) | descriptor heaps as descriptor-buffer memory written once, bound once, indexed per table | option 4 |
| Zink (MIT) | per-program templates, only changed sets written; static bindless buffer plus per-batch descriptor buffer | options 1, 4 |
| Ryujinx (MIT; our shader translator derives from it) | threaded renderer, texture descriptor short cache, buffer views invalidated by generation | option 2 |
| yuzu / Eden / suyu (GPL-3, ideas only) | compile-time method → dirty-flag tables; uniform streaming chosen by hit rate (Eden reverted a blind uniform ring for >10 % regressions) | options 1, 3 |
| Strato (GPL-3, ideas only) | command-node graph, megabuffer reset per fence cycle, per-draw hot cache with sequence numbers | options 1, 3 |
| nxnative (MIT; closest to us, NVN on Metal) | per-stage argument dirty bits set by overlapping uniform writes (`nvn_mtl.m` `g_args_dirty`), per-shader uniform read extents (`cb_extent.tsv`), uniform shadows filled by a copy worker, encode thread | options 1, 3 |
| kirikae (MIT / Apache-2.0) | thread-local direct-mapped address translation cache invalidated by one global `MAPS_CHANGED` counter | option 1 (`find_range`) |
| RPCS3 (GPL-2), Xenia (BSD-3) | table dispatch; no GPU-driven command expansion | rules out option 8 |

Code may be reused only from MIT / zlib / BSD / Apache projects, with attribution; GPL and LGPL sources are ideas only.

## 4. Options, ranked

Savings are estimates ([inference]) against the ~25 ms walker; they overlap and do not add. Each option has a
smallest first step that proves or kills the direction, and an acceptance check on the device.

| # | Option | Estimated walker saving | Cost / risk | Status |
|---|---|---|---|---|
| 0 | Measurements that decide between the options (below) | - | low | **done** (Thor, 2026-10-26): see "Measured on the Thor" |
| 1 | **Resolve once per submission.** Each unique (resource, range, access) read gets one resolve and one frame-lag decision per submission; draws reference the result. Evaluate a last-span cache in front of `find_range`. | original broad estimate 2-5 ms (survey: up to 6-9) | medium | **no demonstrated gain; reverted** (2026-10-26): uniform interning showed no walker gain on the Thor (24.67 vs 24.62 ms profiled; its own lookup cost more); vertex/index/storage admission and a last-span layer were rejected earlier on host cost (below). Individual decisions are cheap; the per-draw structure is the cost (option 2). |
| 2 | **Persistent binding packets.** Per-binding resolved nodes (buffer + offset, view, sampler, uniform version) updated by the change that invalidates them (bind op, inline update, pool / span / registration generation, submit-time deferred-handle refresh); a draw copies a small prebuilt vector. | 2-4 ms (overlaps 1) | medium | **A kept; B, A2 and C rejected** (2026-10-26). A: long frames lower in every Thor pair. A2+C on the Thor: walker on 24.0 / 23.2 vs off 23.1 / 22.6 ms, long frames on 6.4 / 5.7 vs off 4.4 / 4.6 % (worse in both rounds); code reverted, A remains. |
| 3 | **One uniform snapshot per version.** Snapshot keyed (address, shader read extent, dirty generation) once per submission; copy only the reflected extent; fence-retired slab arena instead of ~3,100 allocations a frame. | 1.5-3 ms | low-medium | planned |
| 4 | **Bindless textures and samplers.** Long-lived descriptor arrays written when a pool entry is registered; shaders index by the NVN handle (low 20 bits texture, high 12 bits sampler). | 1-2.5 ms | high (translator, residency, hazards) | later |
| 5 | **Uniforms through device addresses.** Push the uniform slots' device addresses (of the lag-selected snapshot); shaders load through `PhysicalStorageBuffer`, non-writable and 16 B aligned. Lower-risk variant: dynamic offsets into one snapshot arena. | 0.5-2.5 ms | medium-high (translator, seed rebuild) | later. ir3 offline check (grass vertex shader, bundled Turnip, A740/A830): loads become 28 plain `ldg` (no `ldg.k`; SPIR-V globals lack "speculatable" and push-constant addresses are not rematerializable), **all in the per-draw preamble**, which still fills the constant file; the main body is unchanged (326 -> 325 instr on A740), the preamble grows 33 -> 208 (A740) / 84 -> 245 (A830). GPU cost likely per draw, not per vertex [inference, no GPU timing]. Report: `.scratch/xlate-research/ir3-bda.md` |
| 6 | **Static decode off the walker.** Pipeline-key hashing, program resolution, vertex layout and reflection lists computed at EndRecording / flatten on worker cores (not the game threads, which are near budget on the Thor). | 1-2 ms off the serial path | medium | later |
| 7 | ~~Cached secondaries across frames~~ | - | - | **rejected**: 0 of 300 frames repeat the previous frame's recording, even with addresses abstracted |
| 8 | Multi-draw batching of identical-state runs | <0.5 ms walker | low | low priority |

Ruled out: push descriptors (no gain, measured); device-generated commands / GPU expansion of the op stream (not on
Turnip, and the costly decisions are CPU state); full translation at record time on the game threads (deferred binds,
submit-time snapshots, and those threads have no headroom on the Thor); more hash caches in front of existing ones.

### Measurements (option 0), all off by default

1. Why the binding freeze memo misses: `[xlate-freeze]` stage requests/hits/misses and `[xlate-miss]` exclusive
   first-failure counts and counts/frame. Ordered classification: disabled/cold/program/stage, first differing
   consumed binding (deferred handle, texture, sampler, uniform address/size, storage or generated block), uniform
   revision/dirty generation, native lag state, mapping/span, texture registration/lifetime, sampler registration,
   submission, chunk/ticket, dimensions, other. Prior uncacheable writable/unwatched captures are distinguished
   from cold entries. This separates persistent-packet opportunities (option 2) from submission-local work (1/3).
   `direct` requests use immediate recording and do not consult the freeze memo; requests = hits + misses + direct.
2. `[xlate-uniform]` reports one observation per consumed stage/slot/draw, including freeze hits. Fractions compare
   address, bound size, owned selected-content authority/version, observed dirty token, and selected snapshot
   (generation plus descriptor buffer/offset) with the previous draw using that stage/slot and with the previous
   use of that raw address in the same NVN submission. Each fraction names its comparison denominator; zero
   denominator means unavailable, not evidence of unchanged bytes. `unknown` counts unowned/native authority.
   Owned version = submission-snapshot incarnation plus newest overlapping inline-update sequence. The dirty
   token is the existing lag memo's capture observation, **not** a byte digest or precise per-range write counter;
   unrelated writes can change it. No resource bytes are reread for diagnostics. `[xlate-summary]` compares summed
   per-submission unique `(address, reflected extent, owned version)` tuples (`uniform_unique`) with
   `uniform_known` and all `uniform_decisions`. Unknown authority is excluded from the unique-version count,
   not called unchanged. This sizes versioned uniform snapshots (3), binding packets (2), and uniform tables (5).
3. `xlate recordings:` counts submitted flattened recordings and their contained draw operations matching **any**
   recording in the previous real-present frame, separately for byte-exact hashes and hashes with declared address
   fields abstracted (nullness, ordinary payload and resolved deferred handles retained). Repeated occurrences in
   one submission count separately; empty frames clear previous-frame membership. `xlate recording coverage:`
   reports admitted recordings/draws, bytes/ops, unknown schemas, incomplete streams and table overflows.
   Identical 128-bit hashes plus byte/op counts are evidence of repetition, **not** proof that inherited state,
   resource contents, or Vulkan secondaries are reusable (option 7); abstract-only matches help size decode work (6).
   `[xlate-summary] read_unique/read_decisions` sums submission-local dependency cardinality across logical
   uniform, storage, sampled/storage image, texel, vertex and index reads, including bindings hidden by freeze hits
   (option 1). Keys retain read class/access/alignment, raw range or selected indirect-storage descriptor, available
   version observations and mapping/registration generations. They are conservative logical identities: aliases
   are not newly resolved just to count them, and unknown native versions do not prove unchanged content.
4. ir3 disassembly of one translated shader with device-address uniform loads on both GPUs: `ldg.k` vs `ldg`
   (separate experiment; not collected by this environment variable).
5. `[xlate-textures]` gives maximum **used** NVN ID over the run, mean and peak distinct IDs/frame, and unresolved
   view-metadata observations. `[xlate-texture-types]` retains each used ID's cumulative set, grouped by exact
   `(target_mask, view_mask)` at every 3000 frames and normal exit. Target-mask bits 0/1/2 mean storage/texel/sampled;
   bit `target+3` is the reflected requested NVN target; bit 63 is unrecognized. View-mask bit `VkImageViewType`
   is the actual cached selected view type, read without creating/resolving a new view. This sizes option 4's typed
   descriptor arrays; IDs are uses, not a census of every registered but unused pool entry.

Run with **`TOTK_VK_XLATE_STATS=1`** in `totk.env`, restarting the app to change it; no second knob is required.
Keep frame lag and recorder count identical to baseline (`TOTK_FRAME_LAG=1 TOTK_VK_REC_THREADS=2` for the protected
worker path), collect the ordinary route after warmup, and retain all the lines named above. Default-off mode
allocates nothing and adds only a cached flag branch per graphics draw; enabled mode hashes streams and maintains
diagnostic tables, so use a separate stats-only run before collecting timing/profile evidence.
Normal exit emits remaining partial windows; `[xlate-exit] unavailable=walking_active` means the walker was active
at teardown and only its periodic walker reports are valid. A partial window with `frames=0` contains work without
a present, so its per-frame numbers are not frame-rate measurements.

Walker tables have 65,536 slots each and admit 49,152 distinct dependency/address keys per NVN submission;
`overflow` counts rejected insertion attempts. Uniform-version keys share the dependency table but do not inflate
`read_decisions`. Discard cardinality ratios when overflow is nonzero. Recording tables admit 8192 unique keys per
exact/abstract lane/frame, with 16,384 slots; scans are bounded to 64 MiB / 1,048,576 ops. Residual CALL/JUMP streams
are not admitted to the repetition set, though their replayed draws are counted. Hash collisions remain possible;
overflow and `previous_incomplete_*` invalidate negative-match rates, not positive observed hash matches.
The host `vkrectest` runner invokes `vkdrawtest --xlate-stats` in fresh immediate/two-worker processes and checks
actual emitted lines, counts/fractions, same-submission duplicates, address-only differences and empty-frame reset.
Host acceptance (2026-10-26, lavapipe): core/draw/app, recorder cross-mode identities, frozen spans, pipeline and
native-UI-off suites pass both plain and `TOTK_FRAME_LAG=1 TOTK_VK_REC_THREADS=2`. The ordinary draw/app suites
also pass with the diagnostic enabled. The new five-recording fixture emits 2 exact and 3 address-abstracted
previous-frame matches, 5 contained draws, zero hash/schema/table overflows, and one used 2D texture ID in both modes.
These checks establish instrumentation behavior and rendering regressions only; no device timing is claimed.

### Measured on the Thor

Build `apk-xmeasure` (`9892705`, clean tree), Thor, recorded field route, `TOTK_FRAME_LAG=1`, gameplay windows only
(frames 1,500-9,000, 7,500 frames, no table overflow). The diagnostic itself costs ~7 ms of walker time (32.6 vs ~25 ms),
so this run is for counts only.

| Measure | Per frame | What it decides |
|---|---|---|
| Draws / submissions | 3,819 / 1.0 | - |
| Read-dependency decisions / unique within the submission | 47,130 / 6,996 (14.8 %) | option 1: 85 % of decisions repeat one already made |
| Uniform decisions / unique (address, extent, version) | 19,441 / 2,640 (13.6 %) | options 1 and 3 |
| Freeze memo, vertex stage | 15.7 % hits; misses 52 % "uniform address/size" (each draw's per-object block) | option 2: cache per binding, not per stage |
| Freeze memo, fragment stage | 57.7 % hits; misses: texture handle 12 %, program 11 %, uniform address 9 % | option 2 |
| Recordings repeating the previous frame | 0 of 300 (exact and address-abstracted) | option 7 rejected |
| Distinct texture IDs used | ~1,130 (peak 1,212), highest ID 33,023 | option 4: a 32 K-entry array per view type suffices |


### Option 1 implementation and rejected extensions (2026-10-26)

`vk_read_ledger.h` interns protected uniform selections on the walker. The key retains raw alias identity,
canonical physical range, access class (uniform read), requested alignment/extent, mapping/tracker lifetimes,
game submission, actual Vulkan ticket, immutable snapshot incarnation and ordered range revisions. Hits copy
the already-selected descriptor; first use still calls the existing selector and its complete writer-watch path.
Overlapping inline/GPU writes revoke the range, unrelated journal entries preserve it when provable, native CPU
dirty transitions revoke native captures, and mapping/lifetime/ticket/submission changes retire the table. Owned
snapshots deliberately survive unrelated native CPU rewrites. Unwatched copies and pending/live fallback results
are never interned. The table has 16,384 slots, at most 12,288 distinct range/alignment keys per incarnation;
overflow falls back to the existing path and is counted. No worker or protection protocol changed.

`TOTK_VK_LEDGER_CHECK=1` advances an independent legacy memo for each admitted node and compares exact
buffer/offset/range and capture choice. It preserves the interned allocation's identity rather than treating
direct-map eviction and equivalent arena reallocation as a semantic difference. Observed concurrent native-dirty
or mapping transitions return the actual old-path result, revoke the node and increment `races`; they are neither
hits nor successful comparisons. Stable discrepancies abort. `TOTK_VK_OFF=ledger` restores the old uniform path
without disabling protection. With `TOTK_VK_XLATE_STATS=1`, `[xlate-ledger]` reports `uniform_hits`, misses,
invalidations (including scope retirement), overflow, checks, races and decisions avoided/frame every 300 frames.
Keep both diagnostics off for timing.

Host measurements use lavapipe, protected lag/two workers, CPU affinity 0-3/nice 10. Seven interleaved controls
of the actual `vk_lag_uniform_read` API (1M calls/lane, `vkrecspantest --uniform-ledger-bench`) measured median
10.358 -> 9.423 ns/query for one hot key (-9.0%), and 15.944 -> 9.596 for 128 interleaved keys (-39.8%).
Each enabled lane avoids 999,999 / 999,873 decisions, respectively. These isolate selection, **not whole-walk
or Android gains**. Three pairs of six-frame `vkdrawtest-app --lag-walk-bench 6` controls were noisy/neutral:
stable 2054.70 -> 2077.06 ns/draw (+1.1%), churn 2908.53 -> 2926.56 (+0.6%), chunk stress 2218.17 ->
2287.62 (+3.1%); even descriptor-free controls, which never enter this uniform ledger, varied roughly 4-9%.
Stable/churn fixture frames avoid 4,469 / 31,380 uniform decisions with 1,777 / 2,971 misses and no overflow.
No whole-walker host win is claimed; Thor/Odin route A/B and profiles decide acceptance.

Final host acceptance with the independent oracle enabled: core 23/175, draw 103/196, app 109/202,
frozen spans 7097/9619, pipeline 64/64, native-UI-off 16/16 (plain/protected-two-worker); recorder identities
and job/fault/storage/lease suites pass in both modes. The new 1100-range collision case retains exact
descriptors beyond the old memo's capacity. Protected `--lag-walk-bench 6` passes 859 checks; the uniform
microfixture makes 1,999,872 independent clean comparisons. Diagnostics-on draw/app and the 307-check
translation fixture pass both modes. Explicit `TOTK_VK_OFF=ledger` protected controls also pass. Threading
was not changed, so no new TSan claim is made. Device acceptance remains pending.

**Failed extension retained in the record, removed from production:** a general read ledger also admitted
read-only vertex/index/storage. Its descriptor-free device-shaped fixture avoided 18,720 of 23,040 decisions
(81.25%), yet three interleaved two-frame controls measured cold 1246.22 -> 1557.71 ns/draw (+25.0%) and
learned-hot 1276.18 -> 1578.56 (+23.7%). Actual-chunk stress avoided only 2,592/23,040 decisions (11.25%),
1306.06 -> 1900.63 ns/draw (+45.5%). Compact layout/no-mutation fast paths helped the warm case but did not
remove the low-reuse chunk cost. The parent approved rejecting this admission; existing nonuniform geometry,
hot/cold selection, guards and writable SSBO handling remain unchanged.

**Last-span candidate not retained:** `resolve_local` already has generation-keyed per-thread span copies and
64 page hints; dirty `find_range` separately has 256 page-window hints plus eight recent pools. An exact-source
seven-pair microfixture of a kirikae-style generation-keyed last-pool front cache saved only ~0.1 ns on one
repeated pool. Interleaved-pool paired deltas ranged -3.861 to +4.599 ns (median +0.646 ns); four-probe batches
saved a median ~0.17 ns. This does not justify another TLS/generation layer for mixed real draws. No kirikae
code was copied. Sources/logs and the full acceptance report are in `.scratch/xlate-research/option1.md`.

**Device verdict (Thor, `apk-opt1` = `723a466`, same APK with `TOTK_VK_OFF=ledger` as control, recorded route):**

| Run | Ledger on: walker / long frames / fps | Ledger off: walker / long frames / fps |
|---|---|---|
| round 1 | 20.5 ms* / 8.5 % / 28.87 | 25.6 ms / 9.8 % / 28.77 |
| round 2 | 25.2 ms / 7.7 % / 29.11 | 25.3 ms / 9.6 % / 28.97 |
| profiled | 24.6 ms / 11.8 % / 28.47 | 25.8 ms / 9.2 % / 28.84 |

\* the route diverged into a lighter scene from window 13; windows 1-12 match the control (~25.5 ms).
Profile (60 s each, ms/frame on the walker): total 24.67 vs 24.62; `vk_lag_uniform_read` inclusive 2.29 vs 1.82
(its own bookkeeping costs more than the decisions it saves); `bind_stage` 7.48 vs 7.21. Rejected; the code was
reverted on this branch, the experiment is kept here.

**Where `bind_stage` (7.2 ms) really goes** (ledger-off profile, share of `bind_stage`): texture resolution
`vk_tex_by_id` 38 % (84 % of lookups take `tex_by_id_slow`, whose main cost is `tex_get_inner`, a third of which
is re-uploading CPU-written textures: `memcpy` plus page re-protection); uniform reads `vk_lag_uniform_read` 25 %
(mostly the selection memo); uniform descriptors `ubo_desc` 10 %; template writes 4 %. The per-thread texture-ID
cache is keyed by a global epoch that every texture release or pool change bumps, so it rarely survives a frame
in which the game creates and finalizes textures continuously. This makes option 2's first step concrete: per-ID
generations for the texture lookup instead of the global epoch.

### Option 2, step A: precise texture identity (2026-10-26)

The texture-ID TLS cache now validates registration, texture lifetime/mapping and shared-image generations instead
of invalidating every ID on an unrelated release. Generation cells have stable addresses and are recycled with a
monotonic value, so cached pointers are rejected before dereferencing retired host objects, including across
submissions. Shared-image format upgrades invalidate every alias through the image's stamp; pool removal bumps
only overlapping textures; texture-pool bounds compare that ID's membership, including shrink/grow negative-cache
transitions. The broader recorder/program resource revision remains conservative and unchanged.

Image/view identity is separate from content readiness: the first cached use of an ID in a new submission still
calls the existing texture-content selector. It retains mirrored dirty-page checks, uploads, and switching to
GPU-written shared storage. `TOTK_VK_OFF=texgen` restores global-epoch/submission cache invalidation;
`TOTK_VK_TEXGEN_CHECK=1` compares every stable cache hit with independent registry/texture/view resolution and
aborts on an exact view/owner mismatch. Concurrent observed registration, pool-bound or lifetime changes revoke
the cache entry and return the independently resolved answer instead.

Host lavapipe, affinity 0-3/nice 10, protected lag/two recorders, seven interleaved process pairs:
`vktest --texture-churn-bench` (24 frames, 1,024 stable sampled IDs, 32 unrelated texture creations/releases/frame)
measured median **50.901 -> 5.310 ns/lookup (-89.6 %)**; **786,432 -> 1,024** registry/view slow paths out of
786,432 queries (**100 -> 0.130 %**). Each lane performs exactly one first upload plus 23 CPU-written reuploads,
and reads back current bytes every frame. Creation/submission/readback are outside lookup timing. This deliberately
churn-heavy fixture demonstrates invalidation cost, not whole-walker/device speed. One no-churn control measured
5.19 -> 6.65 ns/lookup hot, 9.95 -> 8.55 wide: two lifetime checks have a real hit cost, not a universal cache win.

Core/draw/app/frozen-span/pipeline suites pass plain and protected-two-worker with the independent oracle, as do
recorder cross-mode identities/job/fault/storage/lease fixtures and native-UI/thread-placement/canvas tests.
Core counts are 23/175, draw 103/196, app 109/202, frozen spans 7097/7416 and pipeline 64/64
(plain/protected). The six required suites also pass both modes with `TOTK_VK_OFF=texgen`. Dedicated texture
cache, shared-image replacement and 827-check churn fixtures pass with the oracle in both modes. No thread
ownership/protection protocol changed; no new TSan claim. Full evidence and device A/B instructions:
`.scratch/xlate-research/option2.md`. Thor subsequently kept A on long-frame evidence (results log); Odin and pass-dump equivalence remain pending.


### Option 2, step B: upload/protection proof rejected; stop gate (2026-10-26)

`upload_inner` still runs on the walker for a mirrored texture's first use and, at most once per texture/submission,
when its tracked CPU pages changed (untracked textures conservatively reupload). This is not one upload per draw.
`vktest --texture-upload-bench` exercises eight real 256x256 RGBA block-linear textures over 128 frames per lane:
sparse one-byte CPU writes and full rewrites both cause exactly **8 uploads/frame**, while 32 additional reads/ID
cause none. GPU readback checks both changed and preserved texels. This is a synthetic workload, not the game's
unknown exact upload count/frame. The existing tracker already retains readonly state and batches contiguous
newly writable pages; 1,000 repeated watches of a current texture perform **zero additional mprotect calls**.

A measured candidate (`TOTK_VK_OFF=texprotect` during the experiment) loaded existing page state before attempting
the CAS, avoiding failed RMWs on still-readonly pages while retaining the original ownership/rearm protocol.
Nine interleaved protected-two-worker process pairs showed **no host upload gain**: median walker CPU cost
**43.395 -> 43.747 us/upload sparse**, **43.286 -> 48.412 full**; paired median changes **+7.13 % / +8.89 %**,
with regressions in 6/9 and 8/9 pairs. An isolated redundant-watch microcase got cheaper, but real upload work did
not. Syscall counts and pages were identical (sparse 1,048 calls/16,768 pages; full 4,096/65,536 per 1,024 uploads).
The candidate and its temporary off switch were **removed**, leaving no dirty-tracker/worker change in production.

The retained 2,571-check upload fixture and all six required host suites pass plain and protected-two-worker after
removal, with the step-A oracle on for the suites. No new TSan claim: the final tree changes no threading or page
protocol. Moving deswizzle/copy to a worker was not claimed safe: it requires captured byte authority and explicit
upload-before-draw dependencies; merely reading mutable guest bytes later is not an equivalent implementation.
The initial assignment stopped before C at this no-host-gain gate. After the Thor kept A on long-frame evidence,
the owner explicitly resumed the experiment with A2 (cheaper ready hits), then C (per-binding nodes). This does not
revive the rejected upload shortcut. Candidate/per-pair evidence remains in `.scratch/xlate-research/option2.md`.

### Option 2, A2: completed texture readiness and one lifetime check (2026-10-26)

The texture now release-publishes an atomic `ready_sub` only after its existing content check/upload completes;
cached IDs acquire-read it on a new submission and avoid `tex_get_inner`/`g_tex_mu` when another ID or path already
checked that texture. The old `csub` assignment precedes upload and is deliberately **not** used as completion
authority. Pool invalidation clears readiness; lifetime checks still precede every cached-object dereference.
Rare image upgrades publish their change into each sharing texture owner's stamp under the texture lock, so
the new hit validates one lifetime cell instead of two. The old image stamp remains for the A-only control.
Constant-specialized lookup modes eliminate legacy/fast feature branches inside the hit. Packed multiword keys
were measured and discarded: constructing them cost more than the saved comparisons.

`TOTK_VK_OFF=texready` restores A's two-cell/per-ID readiness path, retaining precise texture generations.
`TOTK_VK_OFF=texgen` still restores the pre-A global epoch; `TOTK_VK_TEXGEN_CHECK=1` remains an independent oracle.
Seven interleaved protected-two-worker host pairs: churn median **4.986 -> 4.327 ns/query (-13.2%)**, hot
**4.25 -> 4.25**, wide **4.41 -> 4.26** (small/noisy). Churn readiness API calls **23,552 -> 0**: another fixture
path has already completed each texture's readiness, so no ID needs to repeat it. Actual upload count remains
**1 + 23**, and each frame's pixels remain current. This is not a claim of zero dirty checks or universal hot-hit
speedup. The replacement fixture now has two distinct texture owners sharing one image and verifies both views
refresh when it is upgraded.

All six required host suites pass plain/protected-two-worker with the oracle, and with `TOTK_VK_OFF=texready`;
cache/churn/replacement/upload fixtures and native-UI also pass. Both-mode TSan builds/suites plus churn and
replacement pass with `LP_NUM_THREADS=0 TSAN_OPTIONS=halt_on_error=1` (the documented lavapipe workaround), no
suppressions. No new worker ownership or protection protocol. Host proof is positive; Thor/Odin A2 timing and
profile (`tex_get_inner`, mutex work, lookup self), pass dumps and real texture transitions decide device acceptance.

### Option 2, C: per-binding selected descriptor nodes (2026-10-26)

Recorder stage entries now retain each consumed UBO's selected buffer/offset/range and guard intent, or the
sampled view/sampler payload. A changed object UBO rebuilds that binding without re-resolving unchanged
textures/samplers or other proven-current UBOs. Exact guest binding values are still compared, including
submit-time deferred handles; there is no new hash lookup or hit allocation. A completely unchanged stage
returns its existing chunk-local set before clearing/materializing the old large freeze key: its hit does not
pay a second cache layer.

UBO nodes retain the old submission/chunk/frozen-span scope, CPU dirty/tracker lifetime and ordered uniform
revision/range checks; generated blocks retain their own semantic inputs. Images/samplers retain submission,
registration/lifetime/native-view/sampler checks and can survive a recorder chunk, but re-resolve at the next
submission for content readiness. Payloads never contain descriptor-slab offsets. Writable storage/image/texel
bindings still resolve and record their ordered effects every draw. Their presence does not revoke unrelated
image/sampler nodes; unwatchable captures revoke uniform-node admission. Whole-stage freezes remain disabled
when the original cacheability rules disallow them. Resource revision is still conservative: an unrelated
registration/release may refresh sampled nodes, while A/A2 keep the underlying ID lookup cheap.

`TOTK_VK_OFF=bindpack` restores the previous whole-stage-only freeze and binding walk, leaving A2 enabled.
`TOTK_VK_BINDPACK_CHECK=1` independently runs the existing resolver for prospective per-binding hits and compares
exact selected descriptors/guard intents or view/sampler handles. Stable disagreement aborts; observed authority
transitions discard admission. The diagnostic is not used for timings.

New `vkdrawtest-app --binding-packet-bench`: four real UBOs and sixteen consumed texture/sampler bindings,
4,096 distinct output pixels/frame, two warmups plus six measured frames per stable/per-object-UBO/writable
case. The writable case checks all 4,096 fragment atomics execute; the isolated exact-hit microcase runs the
real reflected stage resolver in an actual open pass. Seven alternating protected-two-worker controls:

| Host measure | Previous freeze | Binding nodes |
|---|---:|---:|
| Exact unchanged-stage hit, ns | 10.450 | 4.359 |
| Stable CPU ns/draw | 947.855 | 950.038 |
| Per-object UBO CPU ns/draw | 1696.434 | 1678.462 |
| Writable CPU ns/draw | 1476.052 | 1501.926 |
| Stable / changing UBO bind ns/draw | 50.472 / 1161.450 | 43.674 / 1135.530 |
| Writable bind ns/draw | 737.156 | 768.733 |

The hit improves in all seven pairs (**58.3% cheaper at the medians**); changing-UBO CPU improves in 6/7 (paired median -2.45%).
Stable whole-walker cost is neutral; **writable cost regresses** in 6/7 (paired median +0.95%). Removing lookup
decisions is not a universal win: the latter is an explicit device risk, not hidden by the positive microcase.
Texture lookups/draw fall **16 -> 0.00390625** in changing/writable cases, and **0.133 -> 0.00390625** stable.
This is a synthetic target proof, not a prediction of the game's ms/frame.

All six required suites pass plain and protected-two-worker with both node/texture oracles and again with
`TOTK_VK_OFF=bindpack`; native-UI passes too. The new 83-check fixture (all pixels/atomics) and mixed 859-check
lag-walk safety fixture pass with the node oracle. Final TSan builds run all six suites plus the new fixture in
both modes with `LP_NUM_THREADS=0 TSAN_OPTIONS=halt_on_error=1`, both oracles, no suppressions: all pass.
Source and per-pair evidence: `.scratch/xlate-research/option2.md`. A2 and C are separately signed experiments.
Device C A/B: default vs `TOTK_VK_OFF=bindpack`, A2 on in both; check `bind_stage`/uniform/texture/sampler work,
long frames, writable-heavy scenes, and byte-identical pass dumps. To isolate A2 in the combined tree use
`bindpack` vs `bindpack,texready` (or its standalone commit `4014ed7`).

**Device verdict for A2 + C: worse; code reverted** (Thor, `apk-opt2c` = `bdea8c5`, same APK with
`TOTK_VK_OFF=bindpack,texready` as control, step A on in both arms, two interleaved rounds): walker on 24.0 / 23.2 vs
off 23.1 / 22.6 ms, long frames on 6.4 / 5.7 vs off 4.4 / 4.6 %, fps on 29.20 / 29.40 vs off 29.42 / 29.46. Both
steps were reverted together; their designs and host numbers above are kept as the record.

### Option 6 design: parallel submission walking (2026-10-26)

Research only, extending option 6 beyond static decode: **do not split the stream at SET_RT and run concurrent
`mtl_exec` calls**. Actual pass boundaries depend on uniform materialization, resource hazards and conditional
draw eligibility; overlays, GPU-write/snapshot invalidation, texture publishing, counters and actual lag tickets
carry ordered state across passes. The existing packet builder and many resolver memos also have one walking owner.
Keep an ordered semantic front end, give helpers immutable owned-snapshot/binding recipes and private arenas/chunks,
then publish packets in draw order to the existing two recorders. First proof: one helper resolves owned UBOs in
32-draw batches; native/GPU-produced/indirect inputs and all ordered effects retain the original owner path.

Offline opt2A profiles still place ~77.5% in graphics, but that is not a parallel fraction. **[INFERENCE]** a safe
payload split transfers ~3.5-6 ms (14-24%); a streamed owner-plus-helper pipeline models ~20.8 ms centrally on a
25 ms baseline (19.8-23 ms range). Three walker-side threads remain front-end bound. Even an unproven full-versioned
70%-parallel rewrite models ~19.3/17.3 ms for two/three Thor walkers after heterogeneous-core/overhead allowances:
no demonstrated path to 16.7 ms. Bindless offers less (~1-2 ms [INFERENCE]) but deletes work without another core.
Require exact canonical packet/effect traces and byte-identical pass outputs on the actual worker path: ordinary
pass dumps currently select inline recording and alone cannot prove worker equivalence. Apply the existing device
stop gate before expanding the proof; no device run or production change is claimed. Full state map, source anchors,
timing assumptions, oracle and risks: `.scratch/xlate-research/parallel-walk.md`.

### Protection cost: event-dirtied uniform preparation (2026-10-26)

The opt2a profiles put vertex/index packet building at 4.3-4.6 ms/frame inclusive (mostly lag
selection), `draw_touch` at 3.2-3.4 ms, uniform selection at about 1.7 ms, submit capture at
about 1.0 ms, and `vk_lag_uniform_prepare` at about 1.4 ms **inside** `draw_touch`.
These overlap: do not sum them. Page-protection syscalls are already batched/sticky; the profiles'
`mprotect` time also includes simpleperf mmap-event work. The first proof therefore targets the
number of pre-render uniform preparations, not another lookup cache or a different page protocol.

Two 16-bit stage masks retain which bound UBO ranges still need preparation. A changed UBO bind
sets its bit; a new inline overlay, snapshot reset, original-memory GPU producer or mapping
generation dirties both masks. Unconsumed registers stay dirty across shader changes. Preparation
uses the full bound range independently of shader reflection. CPU writes and GPU completion cannot
introduce an original-memory producer, so they do not revoke **this materialization-only proof**.
Actual lag selection, immutable snapshot authority, all-writer watches and guard registration still
run unchanged; the proof must never be reused to skip those mechanisms.

`TOTK_VK_OFF=prepare` restores preparation on every consumed UBO. `TOTK_VK_PREPARE_CHECK=1`
executes legacy preparation for every skipped register and aborts if it materializes any update.
The protected two-worker fixture made **1,912,368 successful independent skipped-call checks**
(1,599,992 in its isolated microfixture, 312,376 in its draw/safety cases). Safety cases cover
inline overlays, GPU producers, aliases, same-address CPU reuse, chunk retirement and unwatched pointers.

Seven interleaved lavapipe process pairs, affinity 0-3/nice 10, `--lag-walk-bench 6`, measured
the isolated eight-range prepare path at median **70.412 -> 4.969 ns/draw**. Whole-walker thread CPU:
stable bindings **2043.08 -> 1997.22 ns/draw** (paired median -4.01%, improved 6/7 pairs),
binding churn **2943.11 -> 2929.16** (paired median -1.96%, improved 5/7), separate actual-chunk
stress **2356.22 -> 2208.49** (paired median -8.37%, improved 6/7).
All **252** corresponding per-frame pixel-hash observations match. Excluding the microfixture,
preparations fall **757,960 -> 197,056 (74.0%)**; the descriptor-free controls are noisy and show no
consistent benefit, as expected. These are synthetic host results, not Android timing.

Core/draw/app/frozen-span/pipeline/native-UI-off suites pass plain and protected/two-worker, with
the oracle and separately with the off switch. Recorder identities, job/fault/storage/lease,
canvas and placement suites also pass. The separately imported `1966018` atlas-UV fixture reads
its UVs from a uniform: protected lag renders **192,192,128,255**, whereas the deliberate
`TOTK_VK_LAG_CAPTURE=0` negative control fails with **0,0,128,255**. Same-ID texture-registry
replacement passes both plain and protected; it tests identity, not the memory-race negative control.

Unsuppressed Clang 21 TSan passes the six core suites plain and protected/two-worker, plus app
registry replacement in both modes, the protected atlas race and the protected whole-walker oracle.
The initial plain app run exposed the already-known raw pass-probe UBO diagnostic race; the existing
fix `3aa3fae` was cherry-picked as `fe05e33`, rather than adding a second implementation.
`TSAN_OPTIONS=halt_on_error=1 LP_NUM_THREADS=0` keeps backend workers enabled without driver worker
noise. No new threading, watch, protection or retirement protocol was introduced.

**Device verdict: no gain; code reverted** (Thor, `apk-prot1` = `8ca5e3d`, same APK with `TOTK_VK_OFF=prepare`
as control, two interleaved rounds): walker on 23.3 / 23.2 vs off 23.5 / 22.6 ms; long frames on 8.2 / 6.5 vs
off 6.5 / 5.8 %; the first nine windows (identical route segment) match within noise. The 4 % host gain did not
carry over. The UI-flash fixtures (`6eba0aa`) and the pass-probe race fix (`fe05e33`) stay.

**Lesson across options 1, 2A and this step:** each made one per-draw mechanism cheaper and each moved the
device walker by at most ~0.5 ms; host fixtures (~2 us/draw) overstate per-mechanism shares of the device's
~23-25 ms. The walker's cost is spread over many small items on one thread; the remaining lever with a chance at
the 16.7 ms target is structural parallelism (option 6: walk render passes on several threads from snapshotted
pass-entry state).

Original device plan for this step:
on two interleaved Thor rounds; check walker/long frames and the targeted profile, then Odin's
UI-flash route and pass-dump equivalence. Only this first mechanism is changed; no COW, vertex/index
selection cache, or page-protection change is claimed. Full buckets, invalidation anchors and host
evidence: `.scratch/protect/report.md`.

### When to stop

Target: walker at or below **16.7 ms/frame on the Thor's route** with protection on, which leaves the 30 fps game
headroom and lets the Odin hold 60 where its GPU allows. An option is dropped when its smallest device proof gains
less than 0.5 ms of walker time over two interleaved rounds (or costs frame-time quality), and the program stops
when the target is reached or every remaining option has been measured. Profiles here use frame-pointer call
graphs with many unwinding errors: inclusive shares are approximate, self times exact, and `mprotect` costs under
simpleperf include the profiler's own mmap-event work, so decisions rest on device A/B timing, not on profile
shares alone.

### How each step is accepted

A step lands only if, on the Thor's recorded field route (`thor-route-env.sh`, interleaved with the baseline, at least
two rounds) the walker's ms/frame drops and long frames do not rise, the host suites stay green (including
`TOTK_FRAME_LAG=1 TOTK_VK_REC_THREADS=2`), and an Odin run shows no regression. A profile (`route-play-perf.sh`)
confirms the targeted functions shrank. Rendering must be byte-identical where the design says it is (pass dumps).

## 5. Results log

| Date | Step | Build | Device / route | Walker ms | Long frames | fps | Verdict |
|---|---|---|---|---|---|---|---|
| 2026-10-26 | baseline r13 | `2b14a0d0` | Thor route, 2 rounds | 24.8 / 23.4 | 8.2 / 4.9 % | 28.91 / 29.38 | reference |
| 2026-10-26 | game-thread fixes (gt4, not a translation change) | `c3b89630` | Thor route, profiled | 24.9 vs 24.7 | 9.1 vs 9.2 % | 28.93 vs 28.80 | neutral on Thor; g-main CPU -9 % |
| 2026-10-26 | option 0: `TOTK_VK_XLATE_STATS=1` measurements 1/2/3/5 | `adc2ee9b` (`9892705`) | Thor route, stats only | (32.6, instrumented) | - | - | counts above; option 1 next, option 7 rejected |
| 2026-10-26 | baseline for options 1-2 (`perf/xlate-redesign`) | `adc2ee9b` (`9892705`, stats off) | Thor route, 2 rounds | 25.6 / 25.7 | 8.4 / 8.1 % | 28.89 / 28.90 | reference |
| 2026-10-26 | option 1: uniform read interning (`TOTK_VK_OFF=ledger` control) | `082ec3fc` (`723a466`) | Thor route, 2 rounds + profiled pair | on 20.5* / 25.2, off 25.6 / 25.3; profiled 24.67 vs 24.62 | on 8.5 / 7.7, off 9.8 / 9.6 % | on 28.87 / 29.11, off 28.77 / 28.97 | no demonstrated gain; reverted (`a997818`). *route diverged |
| 2026-10-26 | option 2A: per-ID texture generations (`TOTK_VK_OFF=texgen` control) | `ef786535` (`fdd2c55`) | Thor route, 2 rounds + profiled pair | on 23.6 / 25.1 / 25.8, off 24.0 / 25.7 / 25.7 | on 5.1 / 8.4 / 7.6, off 7.1 / 11.1 / 10.3 % | on 29.35 / 28.94 / 29.05, off 29.07 / 28.47 / 28.75 | **kept on long-frame evidence**: walker change within noise (-0.6 to +0.1 ms); long frames lower in every pair (-2.0 to -2.7 points); profile confirms the slow path fell (9.1 -> 2.7 %). Odin regression check pending |
| 2026-10-26 | option 2B: mirrored upload/protection shortcut | host only | - | - | - | - | no upload CPU gain on host; removed |
| 2026-10-26 | option 2A2: shared completed readiness / folded image lifetime | `4014ed7` | host only | not measured on device | - | - | churn 4.986 -> 4.327 ns/query; repeated readiness calls removed; hot neutral; normal/TSan suites green |
| 2026-10-26 | option 2C: per-binding selected descriptor nodes | signed C commit | host only | not measured on device | - | - | exact hit -58%; changing-UBO fixture small gain; writable +1.8% risk; normal/off/oracle/TSan suites green |
| 2026-10-26 | protection cost: event-dirtied uniform preparation (`TOTK_VK_OFF=prepare` control) | `0c7c47bd` (`8ca5e3d`) | Thor route, 2 rounds | on 23.3 / 23.2, off 23.5 / 22.6 | on 8.2 / 6.5, off 6.5 / 5.8 % | on 29.13 / 29.32, off 29.14 / 29.49 | no device gain (host -4 %); code reverted |
| 2026-10-26 | thread pinning: `TOTK_AFFINITY_WALK=7`, recorders/game `3-6` | apk-r14 | Thor route, 2 rounds | -0.2 to -0.4 ms | not better | - | no gain; walker already 67 % on mid cores, 33 % on the X3, never on little cores |
| 2026-10-26 | option 6 first proof (owned-UBO helper) | not built | Thor route counters (`t-renv-nxon-2`, frames 7201-7500) | - | - | - | **rejected before implementation**: of 14,300 uniform requests/frame, 4,053 miss the memo and 3,816 of those are direct snapshot binds (a descriptor, no copy); only 82/frame need a covering copy. The cost is the selection (key, pending producer, `snap_find`), which must stay on the ordered owner. Movable work is far below the 0.5 ms gate. Of the 5.67 MB/frame `snap_bytes` (all snapshot allocations), about 5.1 MB is the submit-time UBO capture (1,022.6 KiB in 1,308 ranges per submission, 5 chunks/frame), not uniform reads |
| 2026-10-26 | nxnative ports: hoisted deswizzle + 8-byte hash key compare (`TOTK_FASTSWIZZLE=0 TOTK_HMAP_WORD=0` control) | `981abf79` | Thor route, 2 rounds | on 25.18 / 24.29, off 25.16 / 24.39 | on 17.7 / 15.1, off 12.8 / 17.1 % | on 26.21 / 27.07, off 27.13 / 27.79 | no gain (host 2-4.7x faster did not carry over); reverted from `integ/round14-all` |
| 2026-10-26 | walkfix (GPU-write index, single UBO mapping, find_range query hints) | `a16a455e` (`f03d637`) | Thor route, 2 rounds | 24.4 / 24.0 vs base 22.5 / 24.8 | 9.0 / 13.0 vs 15.8 / 13.0 % | 25.27 / 27.90 vs 27.51 / 28.04 | no detectable change; DWARF profile showed `find_range` untouched (its cost was hint-missing hits, not registration queries) and the GPU-write scan worth only ~0.1 ms on current code |
| 2026-10-26 | walkfix4 = + radix capture sort + sorted lock-free live-range index + conditional recorder signalling | `f9960535` (`dba92af`) | Thor, DWARF on/off-CPU walker profile (20 s) | - | - | - | mechanism confirmed: `find_range` 3.09 -> 0.72 % of walker time, `qsort` 0.68 -> 0.12 %; kernel wake path unchanged (2.3 %: workers really sleep, so signals are needed) |
| 2026-10-26 | descriptor stock (`TOTK_VK_DESC_STOCK`, recorder side) | `11681ef4` (`5d5561e`) | Thor route, 2 rounds | 25.2 / 25.3 vs base 22.5 / 24.8 | 17.0 / 13.3 vs 15.8 / 13.0 % | 26.49 / 27.88 vs 27.51 / 28.04 | no detectable gain; not adopted |
| 2026-10-26 | **walker bundle**: walkfix4 + ThinLTO + PGO (profile from Thor route, r14 code) + walker CPU metric | `319537d4` (`018d0b6`) vs `apk-walkcpu` `04261744` (`1e51710`) | Thor route; 1 confirmation round + 1 profiled + 1 counter run per arm | walker **CPU per draw** base 5.48 / 5.41 us (4,014 / 4,098 draws) vs bundle 4.46 / 4.58 us (4,277 / 4,439 draws); a light-scene bundle run (2,991 draws) measured 5.48 us | - | base 28.00 / 27.64, bundle 25.93 / 25.14 / 27.12 (different scenes) | **~17 % less walker CPU per draw at comparable scene weight (~3.5-4 ms/frame at ~4,000 draws)**; 2 vs 2 runs, CPU-per-draw is scene-dependent (fixed per-frame costs), so route divergence dominates fps/wall comparisons. PMU (walker, 20 s): base IPC 1.76, 43 % backend-stall cycles; bundle IPC 1.36, 50 %: the walker is memory-bound, instructions fell much more than cycles |
| 2026-10-26 | guest-core arbitration (`TOTK_SVC_PROFILE=1`, with/without `TOTK_READY_CORES=1`) | bundle `319537d4` | Thor route, 1 run each | - | - | - | walker (guest 0x1004, prio 30, core 1 only) waits 1.14 ms/frame in `WaitForAddress` for the game thread's frame hand-off (producer dependency), only ~0.17 ms/frame for its core and ~0.1-0.14 ms/frame dispatch; `READY_CORES` showed no measurable change: rejected (no lever here) |
| 2026-10-26 | third recorder thread (`TOTK_VK_REC_THREADS=3`) | bundle `319537d4` | Thor, DWARF on/off-CPU walker profile, 1 run | - | - | - | rejected: walker waits grew (`submit_join` 2.38 -> 2.85 %, `wait_value_inner` 1.37 -> 1.82 %, `lag_gate_wait` 1.89 -> 3.19 %); the extra worker adds protection-gate contention |
| 2026-10-26 | Android shipping-30 defaults: lag off + hardware GPU time | Thor device comparison supplied by owner | Lookout Landing + route | Lookout 3.70 vs 5.06/5.07 us/draw; route 3.22 vs 4.50 us/draw | Lookout 1.4 vs 5.2/13.2%; route 4.7% lag-off/hardware | Lookout 29.93 vs 29.65/27.71; route 29.36 lag-off/hardware | **adopted**: historical controller drops came from latency-mode GPU-time overstatement, not missing lag; explicit lag/hardware overrides unchanged |

### Android shipping-30 default decision (2026-10-26)

Owner-supplied Thor evidence from Lookout Landing, the scene that originally forced lag on:

| Lookout configuration | Mean fps | Worst window fps | Windows below 29.5 fps | Long frames | Walker us/draw |
|---|---|---|---|---|---|
| Previous default: lag on, latency GPU time | 27.71 | 22.6 | 10 | 13.2% | 5.07 |
| Lag on + hardware GPU time | 29.65 | 28.5 | 4 | 5.2% | 5.06 |
| Lag off + hardware GPU time | 29.93 | 29.6 | 0 | 1.4% | 3.70 |

Route runs agree: lag off + hardware gives **29.36 fps**, minimum **27.3**, **4.7%** long frames and
**3.22 us/draw**, versus lag on + hardware at **4.50 us/draw**, with roughly **4,300 draws/frame**.
The historical near-20-fps controller drops at Lookout were caused by latency-mode reports overstating
GPU time by roughly **9–12 ms**, not by missing lag. Hardware reports roughly **23 ms**, matching
KGSL's roughly **22.7 ms**; these virtual reports are not themselves physical busy-time measurements.

`TOTK_FRAME_LAG` now defaults off everywhere, regardless of FPS mode; only exact `1` enables the
unchanged protected lag/hybrid path. `TOTK_GPU_TIME` defaults to hardware only for Android shipping-30
(`TOTK_FPS60` unset/`0`); FPS60=1 and non-Android/BOTW retain latency. Explicit hardware/latency wins,
unknown nonempty values log and retain latency, and an empty value selects the startup default.
The unchanged chunk rule (600 with resolved lag and no native VMA pressure, otherwise 75) now resolves
to 75 by default. Odin's FPS60=1 plus explicit hardware environment is unchanged.
No device or adb access was performed for this implementation; these measurements are the owner's evidence.

## 6. Sources

Khronos proposals: [descriptor buffer](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_descriptor_buffer.html),
[device-generated commands](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_device_generated_commands.html),
[mutable descriptors](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_mutable_descriptor_type.html),
[buffer device address](https://docs.vulkan.org/guide/latest/buffer_device_address.html).
DXVK: [dxvk_context_state.h](https://github.com/doitsujin/dxvk/blob/master/src/dxvk/dxvk_context_state.h),
[dxvk_bind_mask.h](https://github.com/doitsujin/dxvk/blob/master/src/dxvk/dxvk_bind_mask.h),
[d3d11_buffer.h](https://github.com/doitsujin/dxvk/blob/master/src/d3d11/d3d11_buffer.h).
VKD3D-Proton: [CHANGELOG](https://github.com/HansKristian-Work/vkd3d-proton/blob/master/CHANGELOG.md).
Zink: [zink_descriptors.c](https://gitlab.freedesktop.org/mesa/mesa/-/raw/main/src/gallium/drivers/zink/zink_descriptors.c),
[docs](https://docs.mesa3d.org/drivers/zink.html).
Strato: [megabuffer.h](https://raw.githubusercontent.com/strato-emu/strato/master/app/src/main/cpp/skyline/gpu/megabuffer.h),
[licence](https://github.com/strato-emu/strato/blob/master/LICENSE.md) (GPL-3.0-or-later).
Eden: [reverted UniformRing 8440c20](https://git.eden-emu.dev/eden-emu/eden/commit/8440c2074da7b4c901641dea317f570b256f2c03).
nxnative (MIT, github.com/seedds/nxnative @86d9ba1): `dm/gfx/nvn/nvn_mtl.m` (`g_args_dirty`, uniform shadows),
`dm/gfx/nvn/nvn_rec.h` (encode thread), `dm/games/README.md` (`cb_extent.tsv`).
kirikae (MIT / Apache-2.0, github.com/Cor3Duo/kirikae @aa1d8bf): `crates/rt/src/gpu/mod.rs` (`MAPS_CHANGED`,
`LAST_MAPPINGS`).
Mesa (Turnip) at `adc573c4c7` / `f7c93a1352` / main `ab1502f`: `src/freedreno/vulkan/tu_device.cc`,
`tu_cmd_buffer.cc`, `tu_descriptor_set.h`, `tu_common.h`, `src/freedreno/ir3/ir3_nir.c`.
