#!/usr/bin/env python3
"""Draw the TotK Native launcher emblem (original art: a sky island above twin peaks inside a tick ring).

    python3 make_icon.py [--out totk-native-1024.png] [--size 1024]
Deterministic: pure Pillow vector drawing, no fonts, no randomness. The background stays transparent so the APK
builder's launcher_override() puts it on its dark-teal adaptive background and fits it inside the safe zone.
"""
import argparse
import math
from pathlib import Path
from PIL import Image, ImageChops, ImageDraw, ImageFilter

HERE = Path(__file__).resolve().parent
SLATE, DEEP = (16, 34, 38, 255), (9, 19, 22, 255)
TEAL, TEAL_DIM, GOLD = (95, 242, 200, 255), (43, 168, 140, 255), (226, 182, 92, 255)
SS = 4  # supersampling factor: draw at 4x, then downsample for clean anti-aliasing


def ring(draw, c, r, width, color):
    draw.ellipse((c - r, c - r, c + r, c + r), outline=color, width=width)


def polar(c, r, degrees):
    a = math.radians(degrees - 90)
    return c + r * math.cos(a), c + r * math.sin(a)


def emblem(size):
    n = size * SS
    c, u = n / 2, n / 1024  # u: one unit of the 1024 design grid
    base = Image.new('RGBA', (n, n), (0, 0, 0, 0))
    # Disc with a soft vertical gradient (deep slate at the bottom), clipped to the circle.
    shade = Image.linear_gradient('L').resize((n, n)).point(lambda v: v * 0.55)
    disc = Image.composite(Image.new('RGBA', (n, n), DEEP), Image.new('RGBA', (n, n), SLATE), shade)
    mask = Image.new('L', (n, n), 0)
    ImageDraw.Draw(mask).ellipse((c - 486 * u, c - 486 * u, c + 486 * u, c + 486 * u), fill=255)
    base.paste(disc, (0, 0), mask)

    lines = Image.new('RGBA', (n, n), (0, 0, 0, 0))  # everything that glows
    g = ImageDraw.Draw(lines)
    ring(g, c, 470 * u, round(12 * u), TEAL)
    ring(g, c, 398 * u, round(5 * u), GOLD)
    for i in range(48):  # tick ring: long gold ticks on the cardinal/ordinal points, short teal ones between
        long = i % 6 == 0
        g.line([polar(c, (404 if long else 420) * u, i * 7.5), polar(c, 452 * u, i * 7.5)],
               fill=GOLD if long else TEAL_DIM, width=round((9 if long else 5) * u))
    # Twin peaks: one continuous line-art ridge, with an inner ridge echoing it.
    ridge = [(-265, 230), (-140, 45), (-65, 125), (55, -80), (265, 230)]
    g.line([(c + x * u, c + y * u) for x, y in ridge], fill=TEAL, width=round(26 * u), joint='curve')
    inner = [(-180, 230), (-130, 160), (-95, 192), (55, 15), (185, 230)]
    g.line([(c + x * u, c + y * u) for x, y in inner], fill=TEAL_DIM, width=round(10 * u), joint='curve')
    g.line([(c - 290 * u, c + 262 * u), (c + 290 * u, c + 262 * u)], fill=GOLD, width=round(8 * u))
    # The floating island: a faceted inverted wedge with a flat top, a small orb above it.
    island = [(-150, -250), (150, -250), (110, -205), (40, -160), (0, -118), (-45, -165), (-115, -210)]
    points = [(c + x * u, c + y * u) for x, y in island]
    g.line(points + points[:2], fill=GOLD, width=round(12 * u), joint='curve')  # closed, with round joints
    g.line([(c, c - 236 * u), (c, c - 150 * u)], fill=GOLD, width=round(6 * u))
    g.line([(c - 150 * u, c - 250 * u), (c + 150 * u, c - 250 * u)], fill=TEAL, width=round(10 * u))
    orb = 26 * u
    g.ellipse((c - orb, c - 318 * u - orb, c + orb, c - 318 * u + orb), fill=TEAL)
    x, y = c, c + 302 * u  # a small diamond under the horizon
    g.polygon([(x, y - 18 * u), (x + 18 * u, y), (x, y + 18 * u), (x - 18 * u, y)], fill=GOLD)

    glow = lines.filter(ImageFilter.GaussianBlur(22 * u))
    glow.putalpha(glow.getchannel('A').point(lambda v: v * 0.9))
    glow = Image.composite(glow, Image.new('RGBA', (n, n), (0, 0, 0, 0)), mask)  # keep the halo inside the disc
    out = Image.alpha_composite(base, glow)
    out = Image.alpha_composite(out, lines)
    # Clean outer edge: a hard circular alpha bound slightly outside the teal ring.
    edge = Image.new('L', (n, n), 0)
    ImageDraw.Draw(edge).ellipse((c - 492 * u, c - 492 * u, c + 492 * u, c + 492 * u), fill=255)
    out.putalpha(ImageChops.multiply(out.getchannel('A'), edge))
    return out.resize((size, size), Image.Resampling.LANCZOS)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=HERE / 'totk-native-1024.png')
    parser.add_argument('--size', type=int, default=1024)
    args = parser.parse_args(argv)
    emblem(args.size).save(args.out, format='PNG', optimize=True)
    print(f'wrote {args.out}')


if __name__ == '__main__':
    main()
