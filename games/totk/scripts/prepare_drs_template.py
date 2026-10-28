#!/usr/bin/env python3
"""Prepare the unmodified 1.4.3 Bootup SARC for TOTK_DRS_MIN's runtime transform.

Requires Python 3.14's compression.zstd, as do the other RomFS generators.
No game data is shipped: read the player's verified RomFS and authenticate the
original pack, complete decompressed SARC, graphics member, BYML types and words.
The output is NOT a RomFS overlay and contains no edits. One template supports
any accepted floor; scale changes need no regeneration.

From games/totk:
  python3.14 scripts/prepare_drs_template.py --romfs-dir /path/to/v143/romfs \
      --output /path/to/v143/romfs/bootup-drs-v143.sarc
Runtime: TOTK_DRS_MIN=0.75 [TOTK_DRS_TEMPLATE=/other/bootup-drs-v143.sarc].
The default template is $TOTK_ROMFS/bootup-drs-v143.sarc, or the same name in
extracted/v143/romfs when TOTK_ROMFS is unset. Unset TOTK_DRS_MIN needs no template.
Existing Bootup replacements are explicitly refused when this switch is active;
all unrelated RomFS overlays still apply. Keep the template outside mod folders.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from make_mod_manifest import read_index

PACK = 'Pack/Bootup.Nin_NX_NVN.pack.zs'
PACK_SIZE = 286256
PACK_SHA = 'e551acaff6bf6cbb68fab1ad931adfb9307c21c7671d7474cb6a093252e74441'
TEMPLATE_SIZE = 1795492
TEMPLATE_SHA = '96ca09d2dd2309ce7f1df9d16d699cf0ff231b5f2e47ebeea697243f450389a1'
MEMBER = 'System/GameGraphics/ModelSceneExtensionParam/Default.game__gfx__ModelSceneExtensionParam.bgyml'
MEMBER_SIZE = 12144
MEMBER_SHA = '70b9008243e8c80d6d3aef0272553676eaa0698386755a72fb84142137db91d5'
FLOOR_OFFSET = 0x14a4
FLOOR_WORD = 0x3f4ccccd


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sarc_members(data):
    """All named, non-overlapping member ranges; preserve the original layout."""
    require(len(data) >= 20 and data[:4] == b'SARC' and data[6:8] == b'\xff\xfe', 'not a little-endian SARC')
    header, size, start = struct.unpack_from('<HxxII', data, 4)
    require(size == len(data) and 20 <= header <= start <= size, 'SARC header bounds')
    require(header + 12 <= start and data[header:header + 4] == b'SFAT', 'SARC SFAT missing')
    fat_size, count = struct.unpack_from('<HH', data, header + 4)
    names = header + fat_size + count * 16
    require(fat_size == 12 and names + 8 <= start and data[names:names + 4] == b'SFNT', 'SARC SFAT/SFNT bounds')
    require(struct.unpack_from('<H', data, names + 4)[0] == 8, 'SARC SFNT header')
    names += 8
    members = {}
    for i in range(count):
        _, attr, lo, hi = struct.unpack_from('<4I', data, header + 12 + i * 16)
        at = names + (attr & 0xffffff) * 4
        require(attr >> 24 == 1 and names <= at < start and lo <= hi <= size - start, 'SARC member bounds')
        end = data.find(b'\0', at, start)
        require(end >= at, 'SARC member name terminator')
        name = data[at:end].decode('utf-8')
        require(name not in members, 'duplicate SARC member')
        members[name] = (start + lo, start + hi)
    ranges = sorted(members.values())
    require(all(a[1] <= b[0] for a, b in zip(ranges, ranges[1:])), 'overlapping SARC members')
    return members


def validate_template(sarc):
    require(len(sarc) == TEMPLATE_SIZE and hashlib.sha256(sarc).hexdigest() == TEMPLATE_SHA, 'original Bootup SARC size/SHA-256 mismatch')
    members = sarc_members(sarc)
    require(len(members) == 292 and MEMBER in members, 'Bootup must retain all 292 members')
    lo, hi = members[MEMBER]
    member = sarc[lo:hi]
    require(len(member) == MEMBER_SIZE and hashlib.sha256(member).hexdigest() == MEMBER_SHA, 'graphics member size/SHA-256 mismatch')
    require(member[:4] == b'YB\x07\0', 'graphics member must be little-endian BYML7')
    require(member[0x147f] == member[0x14a3] == 0xd2, 'both handheld scales must be BYML floats')
    require(struct.unpack_from('<I', member, 0x1480)[0] == 0x3f666666 and struct.unpack_from('<I', member, FLOOR_OFFSET)[0] == FLOOR_WORD, 'native .9/.8 handheld words mismatch')
    return members


def read_range(romfs, offset, size):
    """Read originals from either the desktop image or the runtime's split image."""
    image = romfs / 'romfs.bin'
    if image.is_file():
        with image.open('rb') as handle:
            handle.seek(offset)
            data = handle.read(size)
        require(len(data) == size, 'RomFS image ends early')
        return data
    first = romfs / 'romfs.bin.000'
    chunk_size = first.stat().st_size
    require(chunk_size > 0, 'empty RomFS first part')
    blocks = []
    while size:
        part, within = divmod(offset, chunk_size)
        count = min(size, chunk_size - within)
        with (romfs / f'romfs.bin.{part:03d}').open('rb') as handle:
            handle.seek(within)
            block = handle.read(count)
        require(len(block) == count, 'RomFS part ends early')
        blocks.append(block)
        offset += count
        size -= count
    return b''.join(blocks)


def prepare(romfs):
    import compression.zstd as zstd

    files = read_index(romfs / 'runtime-index.bin')
    packed = read_range(romfs, *files[PACK])
    require(len(packed) == PACK_SIZE and hashlib.sha256(packed).hexdigest() == PACK_SHA, 'requires the original authenticated 1.4.3 Bootup pack')
    info = zstd.get_frame_info(packed)
    require(info.dictionary_id == 3 and info.decompressed_size == TEMPLATE_SIZE, 'original Bootup dictionary/content size mismatch')
    dictionaries = zstd.decompress(read_range(romfs, *files['Pack/ZsDic.pack.zs']))
    dictionary = None
    for lo, hi in sarc_members(dictionaries).values():
        candidate = zstd.ZstdDict(dictionaries[lo:hi])
        if candidate.dict_id == 3:
            require(dictionary is None, 'ambiguous dictionary ID 3')
            dictionary = candidate
    require(dictionary is not None, 'Bootup Zstandard dictionary ID 3 missing')
    sarc = zstd.decompress(packed, zstd_dict=dictionary)
    validate_template(sarc)
    return sarc


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--romfs-dir', type=Path, required=True, help='verified 1.4.3 RomFS folder (runtime-index.bin and romfs.bin or split parts)')
    parser.add_argument('--output', type=Path, required=True, help='unmodified template; normally <romfs-dir>/bootup-drs-v143.sarc')
    args = parser.parse_args()
    try:
        sarc = prepare(args.romfs_dir)
        # Refuse accidental replacement of a source image/index or unrelated file.
        if args.output.exists():
            require(args.output.read_bytes() == sarc, 'output already exists with different bytes; choose another template path')
        else:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            with args.output.open('xb') as handle:
                handle.write(sarc)
    except (ValueError, OSError, KeyError, ImportError) as error:
        parser.exit(1, f'DRS template: {error}\n')
    print(json.dumps(dict(status='pass', output=str(args.output), bytes=len(sarc), sha256=TEMPLATE_SHA,
                          members=292, edited_bytes=0, selector='TOTK_DRS_MIN=<finite positive scale below 0.8>',
                          policy='preserve .9 step, other mode ladders and all thresholds')))


if __name__ == '__main__':
    main()
