# Turnip driver build recipe

The Android release bundles a prebuilt Mesa [Turnip](https://docs.mesa3d.org/drivers/freedreno.html)
Vulkan driver as `libtotk_turnip.so`. This directory is the recipe that produces
it: a pinned Mesa base commit, the patch series applied on top, and the scripts
that cross-compile the driver for Android with the NDK.

* Shipped library: `libtotk_turnip.so`
  SHA-256 `9b87afeb3d8d30c6a0ad5aab3e0703bc1500e0321e9f17a922106fa556389ec6`
* Mesa base: `f7c93a1352b578fbea6c249227d44df65b8f6a35` (Mesa `main`, 2026-10-19)
* Patch tip: `3e8621cc0e95a9c106d71da3143773dfba68da99`, tree
  `479b6b28724efc8a39db7b9103e8d7e793775529`
* Configuration: Vulkan only, `freedreno` driver, KGSL KMD, Android API 33,
  NDK r28c, `release` variant (`-O3`, `-Ddebug=false`, `-Dstrip=false`, then
  `llvm-strip --strip-all`).

## Layout

| Path | Purpose |
| --- | --- |
| `patches/*.patch` | The 14 Mesa commits on top of the base, exported with `git format-patch --zero-commit` |
| `build.sh` | Clones and patches Mesa, sets up the NDK cross file, builds and packages |
| `package.py` | Copies the built `.so` to `driver.so`, writes `meta.json`, the adrenotools zip and a hash record |
| `requirements.txt` | Pinned host Python packages (Meson 1.10.2, ninja 1.13.0, Mako, PyYAML, packaging) |

All mutable state — the Mesa clone, build trees, packages and logs — lives in a
work directory, `TURNIP_WORK`, which defaults to `games/totk/build/turnip`
(gitignored). Nothing is written into the repository or installed on the host.

## Building

Requirements:

* Linux x86_64 host with `git`, `python3` (with `venv`), `ninja` and
  `pkg-config`. Network access is needed once for the Mesa clone and, on
  Debian/Ubuntu, for the pinned build-tool packages.
* Android NDK r28c (`28.2.13676358`) in `ANDROID_NDK_HOME`; the script refuses
  any other NDK revision.
* `bison`, `flex`, `m4` and `glslangValidator`. The script extracts the pinned
  Ubuntu 26.04 amd64 `.deb`s into `$TURNIP_WORK/tools` without root; set
  `TURNIP_TOOL_DEBS` to a directory containing those exact `.deb` files for an
  offline build. Host tools are used only as a documented fallback, and then
  the resulting hash may differ.

```sh
export ANDROID_NDK_HOME=/path/to/android-ndk-r28c
games/totk/android/turnip/build.sh
```

Environment knobs:

| Variable | Default | Meaning |
| --- | --- | --- |
| `TURNIP_WORK` | `games/totk/build/turnip` | Work directory (clone, builds, packages, logs) |
| `ANDROID_NDK_HOME` | required | NDK r28c installation |
| `TURNIP_VARIANTS` | `release relwithsymbols` | Variants to build; `release` is the shipped one |
| `TURNIP_TOOL_DEBS` | unset | Directory of pre-downloaded pinned `.deb` packages |
| `JOBS` | `nproc` | Parallel compile jobs |
| `PYTHON` | `python3` | Interpreter used to create the venv |
| `SOURCE_DATE_EPOCH` | base commit date | Timestamp used for reproducible archives |

On first run the script clones Mesa bloblessly, checks out the base commit,
applies `patches/*.patch` with `git am` and verifies that `HEAD^{tree}` equals
`479b6b28724efc8a39db7b9103e8d7e793775529`. It then creates the venv, builds
`src/freedreno/vulkan/libvulkan_freedreno.so`, installs it into a staging
directory (which strips the link-time RUNPATH to the platform stubs), strips it
and copies it to:

```
$TURNIP_WORK/packages/release/driver.so            # the shipped library
$TURNIP_WORK/packages/release/meta.json            # adrenotools metadata
$TURNIP_WORK/packages/turnip-release.zip           # driver.so + meta.json
$TURNIP_WORK/logs/                                 # configure/build/install logs and hashes
```

`relwithsymbols` additionally keeps a `driver.so.debug` and a
`vulkan.freedreno.so`.

### Reproducibility

The build is intended to be byte-reproducible given the pinned inputs:

* `SOURCE_DATE_EPOCH` defaults to the base commit's timestamp.
* `MESA_GIT_SHA1_OVERRIDE` is set to the patch tip. Mesa embeds the first ten
  digits of `HEAD` in its version string, and `git am` gives the applied commits
  local hashes; pinning the override makes the string `27.0.0-devel
  (git-3e8621cc0e)` exactly as in the reference build.
* `-ffile-prefix-map=$TURNIP_WORK=/turnip` collapses absolute build paths in
  `__FILE__` and debug information, so the hash does not depend on where the
  work directory is.
* The NDK revision, Meson/ninja/Mako versions and the bison/flex/m4/glslang
  packages are pinned.
* The Mesa tree is verified against the recorded tree hash before compiling.

Expected result for the `release` variant:

```
9b87afeb3d8d30c6a0ad5aab3e0703bc1500e0321e9f17a922106fa556389ec6  driver.so
```

`build.sh` prints the computed hash and says whether it matches. A mismatch
means the host toolchain differs from the reference environment (NDK, host
compiler tools or Python packages); the tree hash check rules out a source
difference.

Reproducing the shipped driver from a fresh work directory:

```sh
export ANDROID_NDK_HOME=/path/to/android-ndk-r28c
TURNIP_WORK=/tmp/turnip-work TURNIP_VARIANTS=release \
    games/totk/android/turnip/build.sh
sha256sum /tmp/turnip-work/packages/release/driver.so
```

Verified on 2026-11-03: a fresh `TURNIP_WORK` with a Mesa checkout at the base
commit re-applied the 14 patches, built the `release` variant and produced
exactly the hash above, together with `meta.json`
(`50ae22b34e979f7b130811653862bcdda2011ae928b6887fa09cd0fef3b430dc`) and
`turnip-release.zip`
(`c52319ef686da2bca72f7ac075a3397c23f88c6f7f0c45dc91083ac3636c5e6d`). The same
host, given the previous 13-patch series, reproduced its pinned driver
(`6017dad5…3876`) exactly, so the toolchain matches the reference environment.
(Before `MESA_GIT_SHA1_OVERRIDE` was pinned, the only difference was Mesa's
embedded version string, `27.0.0-devel (git-<HEAD>)`, which carried the locally
re-applied commit hash instead of the tip's.)

## Patch series

The patches are unmodified upstream Mesa commits plus two local commits (the
KGSL slab allocation work and the A722 CCU count), exported with
`--zero-commit` so they do not reference commit hashes that do not exist
upstream:

1. `util: add util_set_current_thread_full_affinity`
2. `vulkan/wsi: don't let x11 swapchain threads inherit a pinned affinity`
3. `vulkan: don't let the queue submit thread inherit a pinned affinity`
4. `util: restore the launch affinity in threads that shed a pinned one`
5. `tu: Drop reference taken with get_vis_stream_patchpoint_cs()`
6. `tu: Split large indirect draws on A8xx`
7. `tu: Split large indirect count draws on A8xx`
8. `nir: add draw_id_offset system value`
9. `ir3: lower draw_id to use draw_id_offset`
10. `tu: rename skip_vfd to indirect in struct tu_vs_params`
11. `tu: split VS driver params constants setup and emission`
12. `tu: upload draw_id_offset through driver_params UBO`
13. `tu/kgsl: suballocate command streams from reusable mapped slabs`
14. `tu: use two CCUs for A722` — the A722 (Retroid Pocket Duo) descriptor had
    one CCU, and therefore one SP, although the GPU has two of each. With one
    CCU every cold start of the game logged a burst of `UCHE: BR_SP` write page
    faults on the Duo; with two it logs none. Only the A722 entry changes.

## Licence

Mesa is licensed under the MIT licence; the driver built here is MIT. Every
Mesa file touched by the patch series is MIT-licensed:

SPDX `MIT` header:

* `src/freedreno/common/freedreno_devices.py`
* `src/freedreno/ir3/ir3_compiler.h`
* `src/freedreno/ir3/ir3_compiler_nir.c`
* `src/freedreno/ir3/ir3_nir.c`
* `src/freedreno/ir3/ir3_shader.h`
* `src/freedreno/vulkan/meson.build`
* `src/freedreno/vulkan/tests/cs_fake_bo.cc`
* `src/freedreno/vulkan/tests/cs_fake_bo.h`
* `src/freedreno/vulkan/tests/cs_slabs.cc`
* `src/freedreno/vulkan/tests/meson.build`
* `src/freedreno/vulkan/tu_cmd_buffer.cc`
* `src/freedreno/vulkan/tu_cmd_buffer.h`
* `src/freedreno/vulkan/tu_cs.cc`
* `src/freedreno/vulkan/tu_cs.h`
* `src/freedreno/vulkan/tu_cs_pool.cc`
* `src/freedreno/vulkan/tu_cs_pool.h`
* `src/freedreno/vulkan/tu_device.cc`
* `src/freedreno/vulkan/tu_device.h`
* `src/freedreno/vulkan/tu_queue.cc`
* `src/freedreno/vulkan/tu_util.cc`
* `src/freedreno/vulkan/tu_util.h`
* `src/util/u_thread.c`

MIT licence text in the file header (Mesa's pre-SPDX style):

* `src/compiler/nir/nir_divergence_analysis.c`
* `src/compiler/nir/nir_gather_info.c`
* `src/compiler/nir/nir_intrinsics.py`
* `src/compiler/nir/nir_opt_preamble.c`
* `src/util/u_cpu_detect.c`
* `src/util/u_cpu_detect.h`
* `src/util/u_queue.c`
* `src/util/u_thread.h`
* `src/vulkan/runtime/vk_queue.c`
* `src/vulkan/wsi/wsi_common_x11.c`

`docs/drivers/freedreno.rst` is documentation without an in-file header; it is
part of Mesa and covered by the project's MIT licence. It does not affect the
compiled driver.

Patch authorship is preserved by `git format-patch`: the affinity and
`draw_id_offset` commits are by Mike Blumenkrantz, Karmjit Mahil and Vamsi
Chitta; the KGSL slab and A722 CCU commits are local TOTK integration commits.
