#!/usr/bin/env python3
"""Render the launcher icon's density PNGs from the app's own artwork into android/apk/launcher/, which is committed so
that building the APK needs no Pillow. Rerun after changing android/app-assets/icon/totk-native-1024.png (needs Pillow):

    python3 games/totk/android/apk/render_launcher.py [--icon PNG] [--out DIR]

Writes res/mipmap-<density>/ic_launcher{,_round,_background,_foreground}.png (build_apk.launcher_override, the same
images build_apk.py --launcher-icon makes), launcher-512.png (store icon) and source.sha256 (the artwork they came from).
"""
import argparse, hashlib, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build_apk  # noqa: E402

ICON = HERE.parents[0] / 'app-assets/icon/totk-native-1024.png'
OUT = HERE / 'launcher'


def render(icon=ICON, out=OUT):
    images, store = build_apk.launcher_override(icon)
    out = Path(out)
    for _, path, data in images:
        (out / path).parent.mkdir(parents=True, exist_ok=True)
        (out / path).write_bytes(data)
    (out / 'launcher-512.png').write_bytes(store)
    (out / 'source.sha256').write_text(hashlib.sha256(Path(icon).read_bytes()).hexdigest() + '  ' + Path(icon).name + '\n')
    return out


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--icon', type=Path, default=ICON)
    ap.add_argument('--out', type=Path, default=OUT)
    a = ap.parse_args()
    print(f'rendered {render(a.icon, a.out)}')
