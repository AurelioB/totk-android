# Optional LSFG backend

Upstream: https://github.com/FrankBarretta/lsfg-vk-android
Pinned commit: `d76e12e657a6b31cd34550d7f8db8625da8479e6`.
Only the native LSFG 3.1 framegen sources and DXBC extraction/translation
boundary are included. No Vulkan WSI layer, GUI, GameNative glue,
LSFG-Android-Application code, WatermelonDS code, DLL or derived shader binary
is included.

## License gate

| Compiled component | Exact pin | License |
| --- | --- | --- |
| framegen and `extract/trans.*` | upstream pin above | MIT, `LICENSE.md` |
| [DXBC/DXVK compiler](https://github.com/PancakeTAS/dxbc) | `78ab59a8aaeb43cd1b0a5e91ba86722433a10b78` | zlib/libpng, `third_party/dxbc/LICENSE.md` |
| DXBC-bundled Khronos SPIR-V/GLSL headers | files at that DXBC pin | MIT-style Khronos permission, original header notices retained |
| [pe-parse](https://github.com/trailofbits/pe-parse) native parser and codecvt | `31ac5966503689d5693cd9fb520bd525a8710e17` | MIT, `third_party/pe-parse/LICENSE` plus per-file notices |
| [volk](https://github.com/zeux/volk) | `be3dbd49bf77052665e96b6c7484af855e7e5f67` | MIT, `third_party/volk/LICENSE.md` |

All are permissive and MIT-compatible. In particular, the pinned DXBC compiler
is not GPL. Upstream toml11 (`be08ba2be2a964edcdb3d3e3ea8d100abc26f286`, MIT)
was audited but is not needed or vendored: our fixed private DLL path replaces
the layer/configuration boundary. Optional upstream tests/docs/GUI dependencies
are not compiled or vendored. Android system Vulkan, libandroid and libdl are
not redistributed here; the app's existing NDK libc++ shared runtime is used
(Apache-2.0 with LLVM exceptions). The compiler host test uses host system
Vulkan/C++ libraries, not packaged Android binaries.

`notices.h` embeds the applicable copyright/license notices in the distributed
plugin, exported by `totk_lsfg_notices()`. Original notices remain beside sources.
Our bridge, loader, build definitions and tests are MIT.

## Local patches and rationale

1. The Android C ABI borrows the renderer's VkInstance/VkPhysicalDevice/VkDevice,
   queue and enabled null-descriptor capability. The isolated volk dispatch
   table loads those exact objects; it never creates, submits to, waits for or
   destroys a second device in the game path.
2. `Context::recordPair` samples the two retained pre-UI scene snapshots directly
   and writes a borrowed RGBA16F storage target. Inputs enter/leave
   SHADER_READ_ONLY_OPTIMAL, output leaves GENERAL for the affine compositor.
   No AHB engine transport, external ownership transfer, conversion or copy
   occurs. The presenter's ordinary AHB output is unrelated and unchanged.
3. Four source-owned contexts retain tensors and descriptor sets. A changed pair
   rebinds the input descriptors with stack writes after that source's full
   continuation retires. Command-buffer views and barrier builders are
   allocation-free during recording; no per-frame semaphores or commands exist.
4. Thirty timestamp markers bracket28 chain passes and final layout restoration.
   The renderer only harvests after the exact covering ticket retires, avoiding
   stale query availability before the next GPU reset executes. All LSFG work is
   already in real-ticket accounting, never added again as external cost.
5. Normal/performance shader selection maps DXBC resources255–279 and
   255–256/280–302 respectively. Performance tensors/descriptors use reduced
   channels. Flow width/height is0.5/0.75/1.0 of input; the internal constant
   is the reciprocal scale. Defaults are normal/0.5 on every device.
   Resource identities/channel counts were cross-checked against the upstream
   interface inventory at
   https://git.lsfg-vk.dev/lsfg-vk-archive/plain/lsfg-vk-backend/src/extraction/shader_registry.cpp;
   no GPL implementation or proprietary bytecode was copied.
6. Each pair binds beta's pre-previous history to the older real features:
   `[older, older, current]`. Both inputs are preprocessed before beta, so cuts
   and skipped ticks cannot expose an unwritten third history.
7. Host tests use native owned and borrowed images, actual shaders read in
   place or MIT mock shaders, poisoning scratch and reading complete outputs.
   The mock validates descriptors/layouts/lifetime/timestamps, not optical flow.
   Legacy vendored transport APIs remain available to the standalone regression,
   but the renderer no longer exposes the old separate-device ABI.
8. `TOTK_LSFG_PROBE=1` enables private, context-owned snapshots of the seven
   R8 mipmaps outputs after each of the pair's two input dispatches. Only those
   seven images gain transfer-source usage, and only when created with the exact
   opt-in value. The normal recording ABI never captures or exports a probe.
9. `TOTK_LSFG_UBO_REFRESH` is ON by default (exact `0` opts out; other values
   refuse). Every pooled 48-byte constant buffer keeps its creation bytes, and
   right after each recorded pair's first LSFG timestamp the pool and each
   generate pass re-write their constant/phase buffers with `vkCmdUpdateBuffer`
   between buffer barriers. On Odin the pooled buffers were observed overwritten
   after upload by an unidentified writer; this restores them before every read
   and is byte-identical on lavapipe. It is a workaround, not a root-cause fix.

`CMakeLists.txt` builds an isolated `libtotk_lsfg.so`. Hidden/version-scripted
symbols and `-Bsymbolic` prevent volk's dispatch globals from interposing the
host renderer's Vulkan functions. The C renderer loads it only after explicit
engine selection and the user's private DLL are present.

## User data and tests

The app reads only `<private files>/lsfg/Lossless.dll`. It must come from the
user's own purchased Lossless Scaling installation; the user is responsible
for its license terms. This code grants no rights to that proprietary software.
Translation uses memory only: there is no exported/derived disk cache.
`shaders.cpp` accepts only the selected normal/performance3.1 DXBC interface,
translates on demand, and shares compiled shader modules across retained contexts.
No proprietary bytes are required to build the universal APK.

Optional host test, run from an out-of-tree scratch directory with the user's
DLL left in place:

```
cmake -S <worktree>/games/totk/android/third_party/lsfg -B <scratch>/lsfg-host \
  -DTOTK_LSFG_HOST_TEST=ON -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build <scratch>/lsfg-host -j4 --target lsfgtest
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  <scratch>/lsfg-host/lsfgtest /absolute/path/to/user/Lossless.dll rgba16f
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  <scratch>/lsfg-host/lsfgtest /absolute/path/to/user/Lossless.dll rgb10a2
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  <scratch>/lsfg-host/lsfgtest /absolute/path/to/user/Lossless.dll rgba8
# Non-64-aligned extents with moving objects touching the image edges:
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  <scratch>/lsfg-host/lsfgtest /absolute/path/to/user/Lossless.dll rgba16f 1280 720
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  <scratch>/lsfg-host/lsfgtest /absolute/path/to/user/Lossless.dll rgba16f 1152 648
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  <scratch>/lsfg-host/lsfgtest /absolute/path/to/user/Lossless.dll rgba16f 960 536
```

For direct same-device coverage add `LSFG_TEST_DIRECT=1`; select reduced channels
with `LSFG_TEST_PERF=1`, and flow with `LSFG_TEST_FLOW=0.5/0.75/1.0`.
For DLL-free execution, compile the MIT `tests/lsfg_mock_{noop,generate}.comp`
with `glslangValidator -V --target-env vulkan1.3` to `noop.spv` and `generate.spv`
inside a scratch directory, then use `lsfgtest mock:/absolute/scratch/path rgba16f`.
Run normal/performance at all three scales, in both ordinary and protected host
arms. Mock output must equal the known per-pixel midpoint and remain independent
of poisoned scratch. No translated user shader is written by these tests.
`LSFG_TEST_DRS_CROP=1` exercises larger allocations with changing valid subrects
(1280x720 allocation:960x536 then800x448), NaN padding, and explicit crop-to-full
pre-UI normalization. The mock checks the normalized pixels exactly; the actual
models check finite complete edges and scratch independence. This models the
accepted post-composite input contract, not permission to sample DRS padding.

The real-DLL test checks translation, HDR/non-HDR pixels, moving midpoints,
finite RGB, complete lower/right edges, repeated pairs and GPU intervals.
Direct mode also borrows the device/images/recording primary and rebinds input
descriptors; transport mode retains the standalone copy regression. Exposed
history/pyramid images are poisoned differently before two identical pairs,
whose complete output must match. The old two-history path changed716634 bytes
at1280x720 under third-history poisoning. Output alpha is not a scene contract:
the affine compositor supplies1.

Each host fence has a120s software-driver timeout; a timeout exits without
destroying in-flight resources. This is not the game's30ms completion ceiling.
Host correctness tests do not qualify Android scanout, device performance or
arbitrary game motion. APP.md documents private-file provisioning and device
profile selection.

## Private mipmaps probe

This is a default-off diagnostic, not a rendering-policy or synchronization fix.
Set the exact literal `TOTK_LSFG_PROBE=1` before creating contexts; unset, empty,
`0`, `01`, `true`, whitespace and other values are off. The renderer selects the
generation using its existing native dump request. The backend ABI does not
select frames itself:

1. `totk_lsfg_probe_prepare` allocates one reusable HOST_VISIBLE transfer
   readback, with fourteen separate four-byte-aligned regions, outside command
   recording. A context created without opt-in cannot be upgraded in place.
2. `totk_lsfg_record_probe` preserves the ordinary pair's28 dispatches and30
   timestamp markers. Immediately after mipmaps dispatch0 and dispatch1, it
   makes their seven shader-written R8 outputs transfer-readable, copies them
   into distinct retained ranges and restores compute access. There are no
   added pre-dispatch barriers/copies. Normal `totk_lsfg_record` remains the
   no-capture path, even on an opted-in context.
3. After the caller's existing covering GPU fence has completed,
   `totk_lsfg_probe_write` maps readback, invalidates the whole allocation when
   noncoherent, and writes fourteen tightly packed
   `lsfg-mipmaps-frame{0,1}-level{0..6}.r8` files plus
   `lsfg-probe-manifest.txt` into an existing private directory. Metadata records
   actual input image/view handles and input format/extent at recording, plus
   each snapshot's dispatch ordinal, output image/view, exact format
   (`VK_FORMAT_R8_UNORM`,9), extent, buffer offset, row bytes and byte count.
   File errors return0 with an error; an unavailable/unrecorded probe is never
   represented as a successful export.

None of these calls submits or waits. Recording allocates nothing. Readback
stays with the retained context until ordinary source-ticket retirement frees
it. Preparing again, rebinding inputs or recording an ordinary pair invalidates
an older capture. With opt-in off, there are no diagnostic allocations, copies,
GPU barriers, waits or per-generation logs. Exports contain pixels and binding
metadata only, never DLL, translated shader, NIR, ISA or RD bytes.

The host `lsfgtest` target also compiles the real C ABI. Add
`LSFG_TEST_PROBE=1 LSFG_TEST_DIRECT=1` and use the actual private DLL with
`rgba16f`. With strict opt-in, this checks all fourteen files byte-for-byte
against separate per-input mipmaps dispatches, exact ordinary/probed generated
output equality, actual rebound image/view metadata, copy ordering, all30
timestamp markers, allocation failure/reuse, file failure, no-capture behavior
and fence-owned retirement. It forces the noncoherent invalidation branch by
masking coherent capability in the host fixture; this does not claim coverage
of a physically noncoherent host allocation. The test writes only temporary
pixels/metadata and removes them on exit. Other probe values exercise strict-off
behavior in that process, so run each value in a separate invocation:

```
env -u TOTK_LSFG_PROBE LSFG_TEST_DIRECT=1 LSFG_TEST_PROBE=1 \
  VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  <scratch>/lsfg-host/lsfgtest /absolute/path/to/user/Lossless.dll rgba16f
for probe in '' 0 01 true '1 ' ' 1' 2 1; do
  TOTK_LSFG_PROBE="$probe" LSFG_TEST_DIRECT=1 LSFG_TEST_PROBE=1 \
    VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
    <scratch>/lsfg-host/lsfgtest /absolute/path/to/user/Lossless.dll rgba16f
done
```

These are runnable host checks, not Android device-run approval or evidence
about device queue ordering, scanout or performance. Existing overlay, DRS,
AA and same-device LSFG prerequisites remain unchanged.
