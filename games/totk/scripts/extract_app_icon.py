#!/usr/bin/env python3
"""The app icon from the game's own control data: the control NCA in the XCI or NSP carries
icon_<language>.dat (a 256x256 JPEG); it becomes build/AppIcon.icns (sips and iconutil, both part of macOS).
usage: extract_app_icon.py <game.xci|game.nsp> --keys <prod.keys> [--title 0100f2c0115b6000] [--output build/AppIcon.icns]"""
import argparse
import subprocess
import tempfile
from pathlib import Path
from extract_exefs import read_at, require, container_entries, common_tickets, nca_inventory
from extract_romfs import index_romfs
from extract_shared_fonts import Slice, load_keys, nca_romfs

LANGUAGES = ['AmericanEnglish', 'BritishEnglish', 'Japanese', 'French', 'German', 'LatinAmericanSpanish', 'Spanish',
             'Italian', 'Dutch', 'CanadianFrench', 'Portuguese', 'Russian', 'Korean', 'TraditionalChinese', 'SimplifiedChinese']


def control_nca(game, keys, title):
    with game.open('rb') as f:
        _, entries = container_entries(f, game.stat().st_size)
        tickets = common_tickets(f, entries)
        records = nca_inventory(f, entries, keys)
        controls = [r for r, _ in records if r['content_type'] == 2 and r['title_id'].lower() == title.lower()]
        require(controls, 'Expected a control NCA for this title in the game file')
        if len(controls) > 1:
            # a card image holding the base game and its update (Breath of the Wild) has a control NCA for each: the
            # update's follows its Program NCA (the one with a BKTR section) in the file; else the last one
            bktr = [i for i, (r, h) in enumerate(records) if r['content_type'] == 0 and r['title_id'].lower() == title.lower()
                    and any(h[0x400 + s * 512 + 4] == 4 for s in range(4))]
            later = [r for i, (r, _) in enumerate(records) if r in controls and bktr and i > bktr[0]]
            controls = later[:1] or controls[-1:]
        return controls[0]['nca_offset'], tickets


def read_control_data(game, keys, title='0100f2c0115b6000', *, temp_dir=None):
    """Read the preferred icon and English NACP title, verifying the control RomFS's IVFC tree."""
    with tempfile.TemporaryDirectory(dir=temp_dir) as tmp:
        image = Path(tmp) / 'control.romfs'
        offset, tickets = control_nca(game, keys, title)
        nca_romfs(Slice(game, offset), keys, image, tickets)
        entries = {e['path']: e for e in index_romfs(image)}
        names = [f'icon_{lang}.dat' for lang in LANGUAGES]
        names += sorted(n for n in entries if n.startswith('icon_') and n.endswith('.dat'))
        icon = next((entries[n] for n in names if n in entries), None)
        require(icon is not None, 'no icon in the control data')
        with image.open('rb') as f:
            data = read_at(f, icon['offset'], icon['size'])
            label = None
            nacp = entries.get('control.nacp')
            if nacp and nacp['size'] >= 0x600:
                # NACP language slots: American English, British English; each is title[0x200], publisher[0x100].
                for slot in (0, 1):
                    raw = read_at(f, nacp['offset'] + slot * 0x300, 0x200).split(b'\0', 1)[0]
                    text = ' '.join(raw.decode('utf-8', errors='replace').split())
                    if text:
                        label = text
                        break
        return data, label


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('game', type=Path)
    ap.add_argument('--keys', type=Path, required=True)
    ap.add_argument('--title', default='0100f2c0115b6000')
    ap.add_argument('--output', type=Path, default=Path('build/AppIcon.icns'))
    a = ap.parse_args()
    keys = load_keys(a.keys)
    with tempfile.TemporaryDirectory() as t:
        t = Path(t)
        icon, _ = read_control_data(a.game, keys, a.title, temp_dir=t)
        (t / 'icon.jpg').write_bytes(icon)
        iconset = t / 'AppIcon.iconset'
        iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                px = size * scale
                name = f'icon_{size}x{size}{"@2x" if scale == 2 else ""}.png'
                subprocess.run(['sips', '-s', 'format', 'png', '-z', str(px), str(px), t / 'icon.jpg', '--out', iconset / name],
                               check=True, capture_output=True)
        a.output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['iconutil', '-c', 'icns', iconset, '-o', a.output], check=True)
    print(f'{a.output}')


if __name__ == '__main__':
    main()
