#!/usr/bin/env python3
"""Make another game's launcher artwork from the app's own emblem (totk-native-1024.png, make_icon.py) by moving its
teal to blue; the gold and every shape stay. Then render it: apk/render_launcher.py --icon <png> --out apk/launcher-<game>.

    python3 recolour_icon.py [--source totk-native-1024.png] [--out botw-native-1024.png]
Deterministic (needs Pillow). Hues from 100 degrees up move: the lines' teal (163) to 194, the disc's slate (190) to 210;
saturation, value and alpha are kept.
"""
import argparse
import colorsys
from pathlib import Path
from PIL import Image

HERE = Path(__file__).resolve().parent


def recolour(source):
    image = Image.open(source).convert('RGBA')
    pixels = image.load()
    cache = {}
    for y in range(image.height):
        for x in range(image.width):
            r, g, b, a = pixels[x, y]
            if not a or r == g == b:
                continue
            if (r, g, b) not in cache:
                h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
                if h * 360 >= 100:
                    h = (194 + (h * 360 - 163) * 0.6) / 360
                cache[r, g, b] = tuple(round(c * 255) for c in colorsys.hsv_to_rgb(h, s, v))
            pixels[x, y] = cache[r, g, b] + (a,)
    return image


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--source', type=Path, default=HERE / 'totk-native-1024.png')
    ap.add_argument('--out', type=Path, default=HERE / 'botw-native-1024.png')
    a = ap.parse_args()
    recolour(a.source).save(a.out)
    print(f'wrote {a.out}')
