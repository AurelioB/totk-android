# Android port (AYN Thor): status and how to test

Goal: run Tears of the Kingdom natively on Android arm64 the way the Mac build does. The game's own ARM64 code runs
on the CPU, a host answers the Switch OS, and NVN graphics are rebuilt on Vulkan instead of Metal. The first target is
the AYN Thor (Snapdragon 8 Gen 2, Adreno 740, Android 13, 8–16 GB).

Current progress, open bugs, what has been ruled out and the diagnostic switches: [DEBUGGING.md](DEBUGGING.md).

You must own the game. Everything is built on your machine from your own cartridge dump and keys. Nothing from the
game goes in this repository.

## Where it stands

| Part | State |
| --- | --- |
| Code rewriter (`third_party/d2r`) | **Done.** `DM_HOST_TP=tpidr_el0` rewrites for Linux / Android. The Mac output is byte-identical to before. |
| Loader, threads, Horizon services, filesystem, audio mixing (`runtime/`) | **Ported.** Builds for arm64 Linux and Android. |
| Audio output | AAudio (low-latency float stereo, reopened on device loss) |
| NVN API layer (`gfx/nvn/hle_nvn.c` and helpers) | **Ported.** Builds unchanged through `android/compat`. |
| GPU backend | **Vulkan, drawing** (`android/gfx/`): the game draws on the Thor with Turnip, about 30 fps in the world. Details: [VULKAN.md](VULKAN.md). `--gpu null` keeps the old null backend. |
| Frontend | **Headless** (`runtime/frontend_headless.c`): runs from `adb shell`, with no window and no input. |
| Android app (APK, window, touch, controller) | **First version** (`--apk`, [APP.md](APP.md)): NativeActivity, picture in the window, gamepad. No touch controls yet. |

What has been verified here, without game data:

- **Fixture modules.** `android/fixture/` builds synthetic main / subsdk0 / sdk modules that use every instruction
  kind the rewriter replaces: x18 reads, writes, branches and `adr`, guest TPIDR_EL0 and TPIDRRO_EL0, `svc`,
  `cntpct` / `cntvct`, an exclusive load/store window through x18, and a `blr x18` call window.
- **Rewriter checks.** In both modes, the structural checker and the emulator equivalence check pass, and the
  mutation negative controls are all caught.
- **The real loader on arm64 Linux (qemu).** It maps, relocates, seals and checks permissions. Its cross-module probe
  passes (4 threads × 1000 calls). The fixture harness passes 168,000 per-thread slot checks on 6 threads with 0
  failures. An image rewritten for the Mac is refused.
- **Page aliasing for virtual pools.** This is `mremap` on shared anonymous memory, tested on a real Linux kernel,
  including its negative control.
- **Build checks.** The whole headless `totk-native` links for arm64 Linux. Every source file compiles for Android
  API 33 against bionic's headers. The NDK link itself has to happen on your PC.

**On the AYN Thor with the real game (1.4.3):** it boots headless and stays in the game loop. One run went 18 minutes and
43,500 frames, at about 39 fps on the null backend with roughly 1,730 draws per frame. Audio plays through AAudio, and
resident memory is about 1.9 GB.

## Building the public APK

Players install one APK and set the game up on the phone (`docs/android-install.md`). The APK is built from this
repository alone: no game file, key or anything derived from them is read or packed.

**Prerequisites** (Linux x86-64): Python 3.12+ with `cryptography` (`pip install cryptography==50.0.2`; Pillow is *not*
needed), git, make, cmake, javac 17+, the Android SDK (platform 33+ and build-tools, found through `ANDROID_HOME`,
`ANDROID_SDK_ROOT` or `~/Android/Sdk`, or `--android-sdk`), the NDK r28 (28.2.13676358), network access for the pinned
sources (opus, libvpx, hactool, capstone, zstd, libwebp), and for the shader translator either the .NET 10 SDK (it is
published with `tools/totk_xlate/publish_native.sh --android`) or an existing build passed with `--translator`.

**Command:**

```
python3 games/totk/android/build_release.py --ndk ~/Android/Sdk/ndk/28.2.13676358 \
    --turnip <path>/driver.so --key release.pem [--key-pass-env KEY_PASS] \
    [--translator games/totk/prebuilt/android-arm64/totk_xlate] [--work <dir>] [-o totk-native.apk]
```

Output: `<work>/totk-native.apk` (default work folder `games/totk/build/release`, git-ignored), about 27 MB.
`build_release.py --audit <apk>` re-runs only the content audit.

**App id.** The app id (package) is `com.aure.totk` (`android/apk/build_apk.py` `PACKAGE`, with `build_android.py`
`APP_PACKAGE` and the Java packages under `android/java/com/aure/totk`). Earlier test builds used `com.bernalr.totk`:
that is a separate app on the device, which this APK installs next to and cannot read. Players move their saves with
*Saves → Export saves* in the old app and *Saves → Import saves* in the new one (`docs/android-install.md`).

**Signing key.** The key is yours and is never generated or stored by the build: a PEM file holding the private key and
its certificate (RSA or EC P-256), or a PKCS#12 file (`.p12` / `.pfx`). Keep it outside the repository; pass its password
in an environment variable named by `--key-pass-env` (it is never put on the command line or logged). Every later
release must be signed with the same key, or Android refuses the update. The APK is signed with the v1, v2 and v3
schemes (check with `apksigner verify -v`). Example throwaway key for testing only:
`openssl req -x509 -newkey rsa:3072 -nodes -keyout k.pem -out c.pem -days 30 -subj /CN=test && cat k.pem c.pem > test.pem`.

**The Turnip input.** The only prebuilt binary in the APK besides the open fonts is the Mesa Turnip Vulkan driver
(MIT), packed as `lib/arm64-v8a/libtotk_turnip.so`. Its recipe is in this repository (`android/turnip/README.md`):
`ANDROID_NDK_HOME=~/Android/Sdk/ndk/28.2.13676358 sh games/totk/android/turnip/build.sh` clones Mesa at `f7c93a1352b5`
into `$TURNIP_WORK` (default `games/totk/build/turnip`, git-ignored), applies the 14 patches in `android/turnip/patches`
(tip `3e8621cc0e9`), checks the resulting source tree, builds Turnip for KGSL with the NDK r28 for API 33 and packages
`$TURNIP_WORK/packages/release/driver.so` (`android/turnip/package.py`). Pass that file as `--turnip`. The build refuses
any other file; check it yourself with `sha256sum driver.so` =
`9b87afeb3d8d30c6a0ad5aab3e0703bc1500e0321e9f17a922106fa556389ec6` (the same pin is in `android/apk/audit_apk.py`).
The build is reproducible: a fresh run of the recipe gives exactly this hash.

**What it does:** builds `libtotk.so` (`VERSION=v143 DEV_BUILD=0`; the runtime reads its game data at start, so there
are no generated headers), libopus / libvpx, lsfg, the setup library with hactool, capstone, zstd and libwebp (fetched
at pinned commits by `android/setup/*/deps.py`), the translator, `classes.dex` (`android/apk/build_dex.py`), fonts,
licence texts (`assets/licenses/`), the shader list (compressed) and the pipeline key list. It uses the committed
launcher images in `android/apk/launcher/`, which are refused when they are older than
`android/app-assets/icon/totk-native-1024.png` (rerender them with `python3 games/totk/android/apk/render_launcher.py`,
the only step that needs Pillow). It then signs and audits the APK (`android/apk/audit_apk.py`): any entry not on the
allowlist, or anything that looks like game data, removes the APK and stops the build.

## Developer build from your game files (Linux PC)

Needs:

- Python 3.12+ (3.14 if the update is an NSZ)
- git, rsync, make, cmake
- clang or gcc
- LLVM tools (`llvm-objdump`, `llvm-objcopy`)
- the Android NDK r26+ (Android Studio's SDK manager or the NDK zip)
- about 45 GB free

On Arch / CachyOS: `sudo pacman -S llvm cmake rsync`, plus the `android-ndk` AUR package or the zip.

1. **Dump the cartridge on your Switch.** Use nxdumptool (XCI with the update partition, which carries the system
   fonts), Lockpick_RCM for `prod.keys`, and the update NSP if you want 1.4.3.
2. **Build:**

   ```
   python3 games/totk/android/build_android.py ~/Games/totk.xci -k ~/keys/prod.keys --ndk ~/Android/ndk/28.2.13676358 \
       --update ~/Games/totk-1.4.3.nsp [--apk [--turnip driver.so]]
   ```

   It runs the same extraction and checks as the Mac build, including its verified hashes. It then rewrites the code
   again for Android and checks that every site matches the verified rewrite. After that it extracts the data, writes
   the game data folder (`game-data/v143`: `runtime-tables.bin`, `romfs-config.txt`, `shared-fonts.txt`, checked
   against the pinned 1.4.3 hashes) and `install.json`, builds libopus / libvpx and the runtime with the NDK, and writes
   `TOTK_android/` next to the game file. `--runtime-only` later rebuilds the runtime (and with `--apk` the app) and
   the game data folder from the earlier rewrite in minutes; `--apk` builds the same app as the public one, debuggable,
   and `install_app.sh data` fills its game folder over USB.
3. **Run it on the Thor.** Enable Developer options and USB debugging, then:

   ```
   sh TOTK_android/push.sh                       # about 17 GB to /data/local/tmp/totk
   adb shell sh /data/local/tmp/totk/run.sh      # log in the terminal and /data/local/tmp/totk/totk.log
   ```

   What to look for:
   - The three `[totk] mapped …` lines.
   - The RomFS check, which takes a while the first time.
   - `ENTER original nn::init::Start`, then the module constructors.
   - `[nvn-null] frame N: … fps` lines once the game presents frames.

   Whatever line it stops at, send `totk.log`. That line is the next thing to fix.

## Native60 fade and cutscene hardening (1.4.3)

When the existing native60 controller is enabled (`TOTK_FPS60=1` on Android),
four checked fade-conversion veneers turn a **positive** step truncated to zero
into one. Paused `+0`/`-0` stay zero; negative steps, ARM saturation and NaN/FPSR
behavior remain the original `fcvtzs`. No general animation clock is changed.

All four original function entries retain **individual diagnostic timestamps**,
but an entry alone no longer admits a floor: ordinary reset, event type-query
and unclassified update work may run outside cinematics. Only the known
`ExecuteCameraMoveCameraAnimationInstructions` AI update at `main+0xa5a188`,
with nonnull instructions (`+0x20`) and completion bit31 clear (`+0x18`), refreshes
the admitted stamp. This is a conservative **scripted-camera-action subset**,
not a universal cutscene detector. The checked completion store at
`main+0xa5a670` clears admission on the same call; null/already-completed entries
clear it too. Otherwise the admitted stamp expires after **200 ms**.

Both the checked guest interval join at `main+0x2db238` and physical presenter
consume that one admitted stamp. They floor at two guest ticks and two physical
ticks on a 60-Hz panel (four on 120 Hz); larger requests remain authoritative.
Expiry or exit restores ordinary native60 selection. Panels not divisible by
30 quantize to their nearest display tick, as with existing `MFX_30`.
`[fps60-cutscene]` transition lines identify every marker source/meaning,
latest stamp/age, admission reason and guest/present decisions. Stable marker
refreshes are coalesced instead of producing per-present log spam.

The four fades stay enabled whenever FPS60 is enabled, **including framegen
and `TOTK_FPS60_HARDENING=0`**. That switch opts out only of diagnostic
markers/camera exit and the cutscene floor. Any nonempty/nonzero `TOTK_FRAMEGEN`
(including `locked`/`hybrid`) also excludes only that floor group, not fades.
`TOTK_FPS60=0` leaves all ten fade/floor patch sites untouched; Android defaults
to this shipping30 profile. Desktop's unset-FPS60 native60 default remains
enabled. Locked real30/interpolated slots, source lifetimes and deterministic/
MetalFX pacing authority are unchanged. All ten patch words, six adjacent
predicate/continuation words and complete executable/data padding are checked
before any member is written; a mismatch stops boot.
`TOTK_FPS60_FADE_CLAMP=0` is the matched A/B control: with the same APK and
veneers, the four fade sites keep their original `fcvtzs` while the floor group
keeps its own gate. The install line then reads `fade clamps OFF`.

The site selection and cutscene policy are adapted from
[TOTKRecompAndroid](https://github.com/mamcjr/TOTKRecompAndroid) at
`cc4f1346773b97ab799d49e2c0c3e1ebbb8318c8`, tools/patches.py and
runtime/nvn/objects.c (MIT, Copyright © 2026 Peiikai, LAS Recomp /
SavageWindRecomp lineage). The complete permission/warranty notice is in
[`third_party/licenses/TOTKRecompAndroid.txt`](../third_party/licenses/TOTKRecompAndroid.txt).
No Nintendo content is distributed.

Developer check (host Clang, Unicorn and `lz4`; uses only your own local dump):

```sh
taskset -c 0-3 nice -n 10 python3 games/totk/scripts/verify_v143_fps60_hardening.py \
  --image /path/to/rewritten/v143/main.patched.bin \
  --nso /path/to/extracted/v143/exefs/main --text-end 0x2bb9c00 \
  --out /tmp/totk-v143-fps60-hardening
```

This checks authenticated original words, every mismatch guard, emitted ARM
veneer behavior/register/flags preservation and the actual NVN interval/period
selectors with a synthetic clock. It covers generic openfield entries without
a floor, active camera admission, same-call completion/null-entry release,
stale expiry, guest-decision publication and stable diagnostic coalescing.
The gate matrix separately checks fade-only versus floor installation across
native30/FPS60/framegen/hardening combinations, including locked and hybrid.
It does not launch the game or qualify every cinematic. Independently verify
Odin native60 openfield without a generic-marker hold, Thor opening entry/exit
cadence and visible fades, plus the locked-profile negative control.

Without any game data, `make -C games/totk/android fps60patchtest TARGET=linux
CC=clang OUT=<scratch>` then `<scratch>/fps60patchtest` runs the production
patcher on a synthetic main holding only the expected 1.4.3 words: the
FPS60/framegen/hardening gate matrix (including `adaptive`), every site's
veneer entry/replay/return, refusal of each single-word mismatch and occupied
padding before any write, and the admitted-stamp presenter floor.

## Test without game data (developers)

```
DM_HOST_TP=tpidr_el0 python3 games/totk/android/fixture/make_fixture.py /tmp/fx      # rewrite + structck + equiv; writes runtime-tables.bin
# loader harness (arm64 Linux or qemu-aarch64); it reads /tmp/fx/runtime-tables.bin through TOTK_GAME_DATA:
aarch64-linux-gnu-gcc -O2 -D_GNU_SOURCE -I/tmp/fx -Igames/totk/runtime -Igames/totk/third_party/d2r_gfx/gfx/nvn \
  -Igames/totk/third_party/d2r_gfx/gfx/nvn/include games/totk/android/fixture/harness.c games/totk/runtime/game_data.c \
  games/totk/runtime/host_tp.c games/totk/runtime/host_os.c games/totk/runtime/hle_stack.c games/totk/runtime/gate.S \
  games/totk/runtime/hle_stack.S -lpthread -ldl -o /tmp/harness && qemu-aarch64 -L /usr/aarch64-linux-gnu /tmp/harness /tmp/fx
python3 -m unittest discover -s tests -p 'test_android_*.py'; python3 -m unittest discover -s tests -p test_release_apk.py
```

## How the Linux / Android differences are handled

- **Thread slots.** Darwin's patched code finds its per-thread values (virtual x18, guest TLS, Horizon TLR, the host
  gate) through `TPIDRRO_EL0`, which points at the TSD array. On Linux that register reads 0. The Android rewrite emits
  `mrs xN, tpidr_el0; ldr xN, [xN, #16]` instead. TP + 16 is bionic's `TLS_SLOT_APP`, free for apps from API 29. It
  holds a per-thread slot block (`runtime/host_tp.c`). Under glibc it is the executable's first TLS word, checked at
  startup. See `third_party/d2r/hostreg.py`.
- **No red zone.** The rewrite parks scratch registers below sp. Every signal handler is `SA_ONSTACK` on a per-thread
  alternate stack (already true for the Mac), so signals never land there.
- **Guest memory.** Guest memory is shared anonymous memory reserved with `MAP_FIXED_NOREPLACE` below 2^39.
  Android's arm64 user space is 39-bit, the same size as Horizon's. Being shared lets NVN virtual pools alias pages
  with `mremap(old_size 0)`; Darwin uses `vm_remap` for this. Freed pages are returned with `MADV_REMOVE`.
- **Other Darwin services** (Mach VM reads, `mach_absolute_time`, QoS, CommonCrypto, Core Audio) have Linux
  counterparts in `runtime/host_os.*`, `runtime/host_thread.h` and the AAudio branch of `audio_mixer.c`. The graphics
  files get theirs from `android/compat/include`.

## Next steps

1. ~~Boot to the game loop headless on the Thor~~ (done).
2. **Vulkan backend** (stages in [VULKAN.md](VULKAN.md)). Replace `nvn_null.c` with an implementation of the `nvn_mtl.h` interface on Vulkan 1.3 (Adreno
   740, with Turnip as a fallback driver):
   - textures and pools via `VK_EXT_external_memory_host` so pools stay zero-copy
   - the Ryujinx translator's SPIR-V used directly (no SPIRV-Cross)
   - render passes as dynamic rendering
   - real occlusion queries; the game culls with them
   - BC textures: Turnip has BC, but Qualcomm's driver may not, so the iOS BC→ASTC path comes along

   `nvn_mtl.m` is the reference. A lot of its 14k lines are Mac-specific tuning and diagnostics.
3. **Shaders.** The translator ships as a macOS NativeAOT program. For Android, publish it for `linux-bionic-arm64`
   (NativeAOT) to translate on the device, or pre-translate the shared shader list on the PC into the build.
4. **The app.** A GameActivity / NativeActivity APK: an `ANativeWindow` for Vulkan, touch controls (the iOS layout),
   gamepad input, the dual-screen bottom panel later, and the game data in the app's external files directory.

Known risks to watch:

- **VMA limits.** Linux limits mappings per process (`vm.max_map_count`, 65530 on the Thor). Per-page dirty tracking
  of the dma-buf pools reached 92k mappings and froze/crashed in the world. Protection now covers 64 KiB units on
  Linux / Android (`TOTK_DIRTY_UNIT_KB`; `TOTK_DIRTY_UNIT` overrides in system pages), with exact-page exceptions for
  GPU rendering write-back. Dev measured ~22k mappings before the write-back integration; re-measure the combined
  build (see `UPSTREAM_MERGE.md` and the upstream history in `DEBUGGING.md`).
- **The 8 GB Thor Base:** the game uses about 4 GB.
