# TotK Native for Android

**TotK Native** (`com.aure.totk`) runs **The Legend of Zelda: Tears of the Kingdom 1.4.3** natively on Android
handhelds with a Qualcomm Adreno GPU. It is not an emulator: the game's own ARM64 code is rewritten once, on your
device, so that it runs directly on the phone's CPU; the Switch system services are answered by a small host, and the
game's NVN graphics are drawn through Vulkan on a bundled Mesa Turnip driver.

One APK for every device. It contains **no game files**: everything game-specific is built on the device from your own
dump. What changed in each version: [CHANGELOG.md](CHANGELOG.md). Full player guide:
[docs/android-install.md](docs/android-install.md).

- [Features](#features)
- [Requirements and tested devices](#requirements-and-tested-devices)
- [Install and set up](#install-and-set-up)
- [Performance (AYN Thor)](#performance-ayn-thor)
- [Known limitations](#known-limitations)
- [Building from source](#building-from-source)
- [Credits](#credits)
- [Legal](#legal)
- [License](#license)

## Features

**Setup on the device, from your own files**
- Two routes: **decrypted files** (the 1.4.3 RomFS image with the update applied, or its split parts, plus the 1.4.3
  ExeFS `main` / `sdk` / `subsdk0`), or **containers** (base game 1.0.0 as XCI/NSP, the 1.4.3 update NSP and your
  `prod.keys`, merged on the device). Every input is checked against the pinned 1.4.3 hashes before anything is
  written; a wrong pick is refused on the spot with the reason (for example *update: expected a BKTR update, not a
  base game*).
- Everything is generated on the device: the game code is rewritten for Android (a C port of the rewriter, byte-identical
  to the reference), the RomFS is copied and verified, indexed, the shared fonts are written from the bundled Noto Sans,
  the Vulkan **shader seed** is translated, and the **second-screen art** is drawn from your RomFS. Each step shows its
  own progress; setup can be cancelled and resumed, and keys are only held in a private temporary file while needed.

**Pre-game menu, presets and SoC profiles**
- Every launch opens a controller-friendly menu: *Start game*, *Graphics* (Performance, Picture and Screen tabs with
  presets), *Controls & audio*, *Game data*, *Saves* and *About*. **+** starts the game from any screen.
- The first start picks the tested profile of your chip, read from Android's exact `SOC_MODEL`. Each Dragonwing part
  shares the profile of its Snapdragon counterpart:

  | Chips | Class | Tested on | Default |
  |---|---|---|---|
  | CQ8725S, SM8750 | Snapdragon 8 Elite (Adreno 830) | AYN Odin 3 | 60 fps at the game's resolution |
  | QCS8550, QCM8550, SM8550 | Snapdragon 8 Gen 2 (Adreno 740) | AYN Thor | Balanced: 30 fps at the game's resolution |
  | CQ7790S, CQ7790M, SM7750 | Snapdragon 7 Gen 4 (Adreno 722) | Retroid Pocket Duo | Light: 30 fps, 3D scene at 960 × 540 |
  | anything else | untested | — | Light, without the *Recommended* badge |

- Render resolution, FSR 1 and Snapdragon GSR upscaling, FXAA / SMAA, shadow quality, 30 / 60 fps and a live
  performance overlay (**−** then **R** while playing). Experimental options (frame generation: locked, adaptive, LSFG
  with your own DLL; the native HUD) are **off in every profile** and can be turned on by hand.
- Rumble on the controller's motors (or the device's), motion aiming from the controller's or the device's gyroscope,
  and the game's text prompts as an Android text dialog.

**Dual-screen companion**
- On devices with a built-in second display (AYN Thor, Retroid Pocket Duo) the second screen shows Link's status, the
  map and the inventory, with every item under its in-game name and description.
- **Item actions** from the second screen while playing: equip and unequip weapons, bows, shields and armor; eat food
  and materials (including energy-recovering ones); drink elixirs; use fairies; take out a material or Zonai device (placed in
  front of Link, as the game's own shortcut does); and, with the bow drawn, fuse a material to your arrows. Each tap does
  what the game itself would do; buttons are greyed out with the reason when the game would not allow it.
- The panel is detected from the displays present, never from the chip; external monitors, cast and overlay displays
  are ignored.

**Storage, SD card and All files access**
- The game data (about 19 GB) lives in the app's private storage (recommended), on the SD card's app folder, or in a
  folder you choose. It can be moved later; the copy is verified before the old one is removed. FAT32 cards get the
  17 GB image in parts automatically.
- Android's *All files access* is offered, never required: with it, setup reads your dump by its path (in parallel,
  nothing copied) and the data can live in a folder of your choice.

**Saves**
- Saves stay in private storage. *Saves* exports to a zip and imports from a zip or folder (this app's exports and
  JKSV, Checkpoint, Ryujinx or yuzu dumps), makes automatic backups before every change, and shows each slot with its
  picture and the time the game saved it.
- An optional **save folder** (for example on the SD card or a synced cloud folder) is kept in sync a few seconds after
  every in-game save; changes made on the other side are brought in, and a conflict asks which side to keep.

## Requirements and tested devices

- Android 13 (API 33) or newer, arm64, a Qualcomm Snapdragon / Dragonwing chip with an **Adreno GPU**. Only Adreno,
  because the game draws through the bundled Mesa **Turnip** Vulkan driver; the devices' own Qualcomm drivers lack the
  memory-sharing extensions the game needs.
- About **40 GB free during setup**, about **20 GB afterwards**.
- Your own dump of the game (cartridge or eShop purchase, dumped on your own Switch).

| Device | Chip | GPU | Android | Second screen |
|---|---|---|---|---|
| AYN Thor | QCS8550 | Adreno 740 | 13 | built-in, companion on |
| AYN Odin 3 | CQ8725S | Adreno 830 | 15 | none |
| Retroid Pocket Duo | CQ7790S | Adreno 722 | 16 | built-in, companion on |

## Install and set up

1. Install `totk-native.apk` from the Releases page (allow installs from your browser or file manager).
2. Open **TotK Native** and choose where the game data goes: internal storage (recommended), the SD card, or a folder
   you choose.
3. Pick the files of **one** route from your own dump (see [Features](#features)).
4. Setup builds everything on the device and ends on *Choose graphics* with your device's profile selected: press **+**
   to play. You can start playing before the optional second-screen art is finished.
5. Export your saves before uninstalling: uninstalling removes them.

Details, every message and every option: [docs/android-install.md](docs/android-install.md).

## Performance (AYN Thor)

Measured on an AYN Thor (QCS8550, Adreno 740, Android 13):

| What | Result |
|---|---|
| Gameplay frame time | p50 ≈ 33.4 ms at 30 fps, the same from internal storage and from the SD card |
| Title screen after launch | about 12.5 s from internal storage, 19–22 s from the SD card |
| Loading a save | 14–16 s from internal storage, 17–23 s from the SD card; reloading in the game about 11 s |
| Setup, container route (files and install on an exFAT card) | game code about 20 s; base + update merge about 12 min; index and fonts about 20 s; second-screen art about 30 s |
| Moving the game data | about 1 min 10 s within internal storage, about 11 min onto the exFAT card, about 5 min back |
| Storage (4 GB write / read) | internal app storage 2,060 / 2,355 MB/s; SD card 46 / 90 MB/s |

On the AYN Odin 3 (Snapdragon 8 Elite class) the default profile runs at 60 fps; the Retroid Pocket Duo (Adreno 722)
uses the Light profile at 30 fps. Per-device open-world numbers (Hateno Village, Lookout Landing, Rotsumamu Shrine,
camera panning, default profiles) are recorded in the
[1.0.0 performance baseline](docs/benchmarks/baseline-1.0.0.md).

## Known limitations

- Game version **1.4.3** only, and only from the two setup routes above. No DLC, online features or amiibo.
- Adreno GPUs only (Mesa Turnip). `TOTK_VK_DRIVER=system` exists for testing, but no current Qualcomm system driver
  passes the game's startup check.
- Devices other than the three above start on the Light profile and are untested.
- Shaders the shared list does not yet name are translated while playing, which can cause a short stutter the first
  time a new place is seen.
- Frame generation and the native HUD are experimental; frame generation is limited by the GPU headroom in heavy
  scenes on the Thor.
- From the second screen, fusing Zonai devices to arrows is not available.
- Saves live in the app's private storage and are removed with the app: export them first. Test builds signed with
  another key cannot be updated in place by a release build.

## Building from source

Players do not need to. The release APK is built from this repository alone; no game file, key or anything derived from
them is read or packed:

```sh
python3 games/totk/android/build_release.py --ndk <Android NDK r28> --turnip <Turnip driver.so> \
  --key <your signing key .p12> --key-pass-env <VAR> -o totk-native.apk
```

See [games/totk/android/README.md](games/totk/android/README.md) for the developer build, the Turnip recipe
(`games/totk/android/turnip/`), the translator (`games/totk/tools/totk_xlate/`) and the tests
(`python -m unittest discover -s tests` and `python -m unittest discover -s tests/setup -t tests/setup`). The macOS
path inherited from MacNativeGames (`build_app.py`) is kept: it shares the runtime and several tests compile against it.

## Credits

This project builds on the work of others:

- **[MacNativeGames](https://github.com/ly05010419/MacNativeGames)** by ly05010419 and its contributors (MIT): the
  native Tears of the Kingdom port for Apple silicon this project is derived from (runtime, Horizon host, rewriter
  integration, shared shader list, tests).
- **[kitcatkode/MacNativeGames](https://github.com/kitcatkode/MacNativeGames)**: the Android port of MacNativeGames
  (Linux / Android runtime, Vulkan backend, first APK) that this Android project started from.
- **[D2R Mobile](https://github.com/jhaizhou-ops/d2r-mobile)** (MIT): the way of running Switch games natively, the NSO
  rewriter (`games/totk/third_party/d2r`), the Horizon host and the NVN graphics layer (`games/totk/third_party/d2r_gfx`)
  started as its generic parts; the translator front end too.
- **Ryujinx** (MIT): the shader translator (`vendor/ryujinx`: Ryujinx.Graphics.Shader,
  Ryujinx.Common, Spv.Generator) and the audio effect reference.
- **[Mesa](https://mesa3d.org) Turnip** (MIT): the bundled Vulkan driver for Adreno GPUs.
- **hactool** (ISC, SciresM) with mbedTLS, cJSON and LZ4; **Capstone** (BSD-3-Clause); **zstd** (BSD-3-Clause);
  **libwebp** (BSD-3-Clause); **libopus** and **libvpx** (BSD-3-Clause); **Noto Sans** (SIL OFL 1.1).
- **lsfg-vk-android** (MIT), **SMAA** (MIT), **AMD FidelityFX FSR 1** (MIT), **Snapdragon GSR** (BSD-3-Clause),
  **deko3d** (zlib), **bcdec**, the NVN API header by h1k421, **nxnative**, **nx2android**, **LASRecomp-Android-Plus**,
  **TOTKRecompAndroid**, texture decoders from texture2ddecoder, AssetStudio, mikunyan, bimg and Pillow, the .NET
  runtime (NativeAOT), SPIRV-Cross and LLVM libc++.

Every bundled component, its version and its licence are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
and shown in the app under *About → Open-source licences*. libvpx and libwebp also carry Google's patent grants
(PATENTS files, included there).

## Legal

**You must own the game.** Use this project only with a copy of The Legend of Zelda: Tears of the Kingdom that you
bought, and with keys and system files dumped from your own Nintendo Switch. This is an independent, non-commercial
project. It is not affiliated with, endorsed by, or sponsored by Nintendo. The Legend of Zelda and Nintendo Switch are
trademarks of Nintendo.

The repository and the APK contain no game content and distribute none: no game program, art, text, audio, shaders or
keys, and they do not help to obtain them. Everything the game needs is made on your own device from your own files.
What the repository does hold is the information needed to make the port work: the patch code, the addresses and hashes
used to check your files, and the [shared shader list](docs/shader-list.md), which names shaders and bind states but
contains no shader code. The game data the app builds is for your own use only; do not share it.

## License

MIT (see [LICENSE](LICENSE)). Third-party code keeps its own license: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
