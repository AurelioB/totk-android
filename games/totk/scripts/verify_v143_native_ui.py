#!/usr/bin/env python3
"""Host-only native UI canvas/scene/heap checks; no Android build or guest entry.

Needs Python3.14 compression.zstd plus lz4/unicorn, a C compiler and player's
1.4.3 NSO/RomFS. All generated fixture files go under --scratch.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn import arm64_const as reg

import fixture_seeds
from prepare_drs_template import read_index, read_range

ROOT = Path(__file__).resolve().parents[1]
# Output-size (5) and render-size (4) sites; SHA-256 of their words (runtime/code_guard.h).
SEED_SITES = [0xbdb578, 0xbdb584, 0x3dcb28, 0x3dcb2c, 0x3dcb30, 0x585e48, 0x585e50, 0x585e54, 0x585e60]
SEED_SHA256 = '1923a1056583193058e5d3c40a6b0913400a771c840b2e03cea57fc7c794326d'


def original_module(path):
    read = fixture_seeds.read_main(path)
    fixture_seeds.check(read, SEED_SITES, SEED_SHA256)
    return read


def heap_sizes(romfs):
    import compression.zstd as zstd
    name = 'System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs'
    index = read_index(romfs / 'runtime-index.bin')
    raw = read_range(romfs, *index[name])
    assert zstd.get_frame_info(raw).dictionary_id == 0
    data = zstd.decompress(raw)
    assert data[:4] == b'YB\x07\0'
    strings, _, root = struct.unpack_from('<III', data, 4)
    count = struct.unpack_from('<I', data, strings)[0] >> 8
    offsets = struct.unpack_from(f'<{count + 1}I', data, strings + 4)
    keys = [data[strings + off:data.index(0, strings + off)].decode() for off in offsets[:-1]]

    def field(node, name, kind):
        assert data[node] == 0xc1
        for i in range(struct.unpack_from('<I', data, node)[0] >> 8):
            at = node + 4 + 8 * i
            key = struct.unpack_from('<I', data, at)[0] & 0xffffff
            if keys[key] == name:
                assert data[at + 3] == kind
                return struct.unpack_from('<I', data, at + 4)[0]
        raise AssertionError(name)

    heaps = field(root, 'Heaps', 0xc1)
    values = {name: field(field(heaps, name, 0xc1), 'Size', 0xd1) for name in ['DynamicTextureAllocatorMem', 'Graphics']}
    assert values == {'DynamicTextureAllocatorMem': 113664, 'Graphics': 201728}
    return dict(original_sha256=hashlib.sha256(raw).hexdigest(), original_heap_kib=values,
                native_ui_1080_scene_960_extra_kib=0, overlay_required=False)




def emulate_scene(image, original):
    code = bytearray(image)
    code[0x585e48:0x585e6c] = original(0x585e48, 0x24)
    for address in [0x585e48, 0x585e50, 0x585e54, 0x585e60]:
        code[address:address + 4] = image[address:address + 4]
    mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    mu.mem_map(0, len(code)); mu.mem_write(0, bytes(code))
    renderer, source = 0x10000000, 0x10004000
    mu.mem_map(renderer, 0x8000)
    mu.mem_write(source + 0x40, struct.pack('<I', 0x1234))
    mu.mem_write(renderer + 0xc38, struct.pack('<Q', 0x5678))
    mu.mem_write(renderer + 0x9f0, struct.pack('<Q', 0x9abc))
    mu.reg_write(reg.UC_ARM64_REG_X19, renderer)
    mu.reg_write(reg.UC_ARM64_REG_X8, source)
    mu.reg_write(reg.UC_ARM64_REG_X9, 1920)
    mu.reg_write(reg.UC_ARM64_REG_X10, 720)
    mu.reg_write(reg.UC_ARM64_REG_X11, 1280)
    mu.emu_start(0x585e48, 0x585e6c, count=16)
    assert mu.reg_read(reg.UC_ARM64_REG_PC) == 0x585e6c
    assert struct.unpack('<ff', mu.mem_read(renderer + 0xcc4, 8)) == (960.0, 540.0)
    assert struct.unpack('<f', mu.mem_read(renderer + 0xcc0, 4))[0] == 1920.0
    assert mu.reg_read(reg.UC_ARM64_REG_X9) == 1920


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--main-nso', type=Path, required=True)
    parser.add_argument('--romfs-dir', type=Path, required=True)
    parser.add_argument('--scratch', type=Path, required=True)
    args = parser.parse_args()
    out = args.scratch.resolve(); assert out != ROOT and ROOT not in out.parents
    out.mkdir(parents=True, exist_ok=True)
    original = original_module(args.main_nso)
    heaps = heap_sizes(args.romfs_dir)
    binary = fixture_seeds.build(ROOT / 'scripts/v143_native_ui_fixture.c', out / 'native-ui-fixture')
    seeds = fixture_seeds.write_seeds(out / 'native-ui-seeds.bin', original, SEED_SITES)
    env = {k: v for k, v in os.environ.items() if k not in ['TOTK_NATIVE_UI', 'TOTK_OUTPUT_RES', 'TOTK_RENDER_RES', 'TOTK_EXPORT_IOS', 'TOTK_ROMFS_PATCHES_OUT']}
    env.update(TOTK_OUTPUT_RES='0', TOTK_RENDER_RES='960x540', FIXTURE_SEEDS=str(seeds))

    def run(extra, arguments=('query',), success=True):
        case = env.copy()
        for key, value in extra.items():
            if value is None:
                case.pop(key, None)
            else:
                case[key] = value
        result = subprocess.run([str(binary), *arguments], env=case, text=True, capture_output=True)
        assert (result.returncode == 0) == success, (extra, result.stderr)
        return json.loads(result.stdout) if success else None

    assert run({})['output_active'] == 0
    assert run({'TOTK_NATIVE_UI': '0'})['output_active'] == 0
    for output in [None, '0', 'auto', '1920x1080', '2560x1440', 'malformed']:
        got = run({'TOTK_NATIVE_UI': '1', 'TOTK_OUTPUT_RES': output})
        assert got == dict(output_active=0, output=[0, 0], scene_active=1, scene=[960, 540], extra_bytes=0)
    shipping_scene = run({'TOTK_NATIVE_UI': '1', 'TOTK_RENDER_RES': None})
    assert shipping_scene['scene_active'] == 0 and shipping_scene['extra_bytes'] == 0
    assert run({'TOTK_NATIVE_UI': '1', 'TOTK_EXPORT_IOS': 'unused'})['output_active'] == 0
    for value in ['yes', '2', '-1', '1 ']:
        run({'TOTK_NATIVE_UI': value}, success=False)
    for index in range(len(SEED_SITES)):
        # The independent OUTPUT_RES utility still guards its original words;
        # native UI itself no longer patches the game's output configuration.
        assert run({'TOTK_NATIVE_UI': '0', 'TOTK_OUTPUT_RES': '1920x1080'},
                   ('corrupt', str(index))) == {'refused_before_write': True}
    dump = out / 'native-ui.bin'
    applied = run({'TOTK_NATIVE_UI': '1'}, ('dump', str(dump)))
    assert applied['output_active'] == 0 and applied['scene'] == [960, 540] and applied['extra_bytes'] == 0
    image = dump.read_bytes()
    assert image[0x3dcb28:0x3dcb2c] == original(0x3dcb28, 4)
    assert image[0xbdb578:0xbdb57c] == original(0xbdb578, 4)
    emulate_scene(image, original)
    report = dict(status='pass', original_word_guards=len(SEED_SITES), output_override_active=False,
                  independent_scene=[960, 540], defaults_and_invalid_values=True,
                  verified_heap=heaps, shipping_canvas_configuration_unchanged=True,
                  scope='production C plus original NSO instructions; no game startup, postprocess GPU cost or device claim')
    (out / 'native-ui-checks.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
