#!/usr/bin/env python3
"""Host-only DRS checks; no guest execution, device, Android build or repo outputs.

Requires Python 3.14 (the production generator), a host C compiler and libzstd.
Compiles the real romfs.c with a tiny external image and production SHA-256,
then independently decodes its output through the installed libzstd. Local
original game data is read only; generated data and binaries stay under scratch.

  python3.14 -B scripts/verify_drs_transform.py --romfs-dir /path/to/v143/romfs \
      --scratch /path/outside/source/drs-checks
"""
import argparse
import ctypes
import ctypes.util
import hashlib
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
import prepare_drs_template as drs


class Zstd:
    """An independent decoder, not the runtime's raw-frame writer."""
    def __init__(self):
        name = ctypes.util.find_library('zstd')
        if not name:
            raise RuntimeError('host libzstd required for independent frame verification')
        self.lib = ctypes.CDLL(name)
        signatures = {
            'ZSTD_getFrameContentSize': ([ctypes.c_void_p, ctypes.c_size_t], ctypes.c_ulonglong),
            'ZSTD_getDictID_fromFrame': ([ctypes.c_void_p, ctypes.c_size_t], ctypes.c_uint),
            'ZSTD_getDictID_fromDict': ([ctypes.c_void_p, ctypes.c_size_t], ctypes.c_uint),
            'ZSTD_createDCtx': ([], ctypes.c_void_p),
            'ZSTD_freeDCtx': ([ctypes.c_void_p], ctypes.c_size_t),
            'ZSTD_decompress_usingDict': ([ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t], ctypes.c_size_t),
            'ZSTD_isError': ([ctypes.c_size_t], ctypes.c_uint),
            'ZSTD_getErrorName': ([ctypes.c_size_t], ctypes.c_char_p),
        }
        for symbol, (args, result) in signatures.items():
            fn = getattr(self.lib, symbol)
            fn.argtypes, fn.restype = args, result

    def decode(self, packed, dictionary=None):
        size = self.lib.ZSTD_getFrameContentSize(packed, len(packed))
        assert size < 64 << 20, 'missing/invalid/oversize frame content size'
        output = ctypes.create_string_buffer(size or 1)
        context = self.lib.ZSTD_createDCtx()
        assert context
        try:
            count = self.lib.ZSTD_decompress_usingDict(context, output, size, packed, len(packed), dictionary, len(dictionary) if dictionary else 0)
            assert not self.lib.ZSTD_isError(count), self.lib.ZSTD_getErrorName(count).decode()
            assert count == size
            return output.raw[:size]
        finally:
            self.lib.ZSTD_freeDCtx(context)


def build_fixture(root, game, cc, legacy=False):
    # Extract just the portable production SHA implementation. Compiling all of
    # host_os.c on x86 would also assemble unrelated AArch64 cache operations.
    source = (game / 'runtime/host_os.c').read_text()
    sha = source[source.index('/* SHA-256 (FIPS 180-4)'):source.rindex('#endif')]
    (root / 'hash.c').write_text('#include "host_os.h"\n#include <string.h>\n' + sha)
    (root / 'totk_ui_lang.h').write_text('#define TOTK_UI(zh,en) (en)\n')
    image_size = drs.PACK_SIZE + len(b"fixture original")   # romfs-config.txt's size, passed to the fixture directly
    target = root / ('fixture-legacy' if legacy else 'fixture')
    command = shlex.split(cc) + ['-std=c11', '-D_GNU_SOURCE', '-O2', '-Wall', '-Wextra', '-Werror',
                                '-Wno-unused-function', '-Wno-misleading-indentation',
                                '-I', str(root), '-I', str(game / 'runtime')]
    if not legacy:
        command.append('-DTOTK_GAME_V143=1')
    command += [f'-DFIXTURE_IMAGE_SIZE={image_size}u']
    command += [str(game / 'scripts/drs_romfs_fixture.c'), str(game / 'runtime/game_data.c'), str(root / 'hash.c'), '-pthread', '-lm', '-o', str(target)]
    subprocess.run(command, check=True)
    return target


def fake_image(root, packed, split=False):
    plain = b'fixture original'
    names = ['Other/plain.bin', drs.PACK]
    strings = b''.join(name.encode() + b'\0' for name in names)
    entries = struct.pack('<QQII', 0, len(plain), 0, len(names[0]) + 1)
    entries += struct.pack('<QQII', len(plain), len(packed), len(names[0]) + 1, len(names[1]) + 1)
    (root / 'runtime-index.bin').write_bytes(struct.pack('<8sQQ', b'TOTKRFS1', 2, 72) + entries + strings)
    payload = plain + packed
    if split:
        # Bootup spans three parts; src_pread must carry a whole-pack hash across them.
        for part, start in enumerate(range(0, len(payload), 100000)):
            (root / f'romfs.bin.{part:03d}').write_bytes(payload[start:start + 100000])
    else:
        (root / 'romfs.bin').write_bytes(payload)
    mods = root / 'mods'
    target = mods / 'Other/plain.bin'
    target.parent.mkdir(parents=True)
    replacement = b'fixture overlay'
    target.write_bytes(replacement)
    (mods / 'manifest.tsv').write_text(f'Other/plain.bin\t{len(replacement)}\t{hashlib.sha256(replacement).hexdigest()}\t{hashlib.sha256(plain).hexdigest()}\n')
    return mods


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--romfs-dir', required=True, type=Path)
    parser.add_argument('--scratch', required=True, type=Path, help='external directory for temporary host builds/data')
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    game = Path(__file__).resolve().parents[1]
    scratch = args.scratch.resolve()
    if scratch == game or game in scratch.parents:
        parser.error('--scratch must be outside games/totk')
    args.scratch.mkdir(parents=True, exist_ok=True)
    decoder = Zstd()
    index = drs.read_index(args.romfs_dir / 'runtime-index.bin')
    packed = drs.read_range(args.romfs_dir, *index[drs.PACK])
    assert len(packed) == drs.PACK_SIZE and hashlib.sha256(packed).hexdigest() == drs.PACK_SHA
    dictionary_pack = decoder.decode(drs.read_range(args.romfs_dir, *index['Pack/ZsDic.pack.zs']))
    dictionaries = [dictionary_pack[lo:hi] for lo, hi in drs.sarc_members(dictionary_pack).values()]
    matches = [data for data in dictionaries if decoder.lib.ZSTD_getDictID_fromDict(data, len(data)) == 3]
    assert len(matches) == 1
    dictionary = matches[0]
    original = decoder.decode(packed, dictionary)
    members = drs.validate_template(original)
    floor = members[drs.MEMBER][0] + drs.FLOOR_OFFSET
    cases = []
    with tempfile.TemporaryDirectory(prefix='drs-', dir=args.scratch) as temporary:
        root = Path(temporary)
        template = root / 'prepared.sarc'
        generated = subprocess.run([sys.executable, '-B', str(game / 'scripts/prepare_drs_template.py'),
                                    '--romfs-dir', str(args.romfs_dir), '--output', str(template)], capture_output=True, text=True)
        assert generated.returncode == 0, generated.stderr
        assert template.read_bytes() == original
        fixture = build_fixture(root, game, args.cc)
        legacy = build_fixture(root, game, args.cc, legacy=True)
        env = {key: value for key, value in os.environ.items() if not key.startswith(('TOTK_ROMFS', 'TOTK_DRS', 'TOTK_FS_'))}
        env['TOTK_ROMFS_PATCHES'] = '0'

        def run(case, scale='0.75', template_bytes=original, source=packed, mmap=False, split=False, bootup_mod=False, old_version=False, custom_template=False):
            folder = root / case
            folder.mkdir()
            mods = fake_image(folder, source, split=split)
            source_template = folder / ('custom-template.sarc' if custom_template else 'bootup-drs-v143.sarc')
            if template_bytes is not None:
                source_template.write_bytes(template_bytes)
            if bootup_mod:
                file = mods / drs.PACK
                file.parent.mkdir(parents=True)
                file.write_bytes(packed)
                with (mods / 'manifest.tsv').open('a') as handle:
                    handle.write(f'{drs.PACK}\t{len(packed)}\t{drs.PACK_SHA}\t{drs.PACK_SHA}\n')
            case_env = env | {'TOTK_ROMFS_MODS': str(mods)}
            if custom_template:
                case_env['TOTK_DRS_TEMPLATE'] = str(source_template)
            if scale is not None:
                case_env['TOTK_DRS_MIN'] = scale
            if mmap:
                case_env['TOTK_ROMFS_MMAP'] = '1'
            output = folder / 'served.pack.zs'
            result = subprocess.run([str(legacy if old_version else fixture), 'serve', str(folder), str(output), 'fixture overlay'], env=case_env, capture_output=True, text=True)
            return result, output, source_template

        result, output, _ = run('default-off', scale=None, template_bytes=None)
        assert result.returncode == 0 and output.read_bytes() == packed, result.stderr
        assert 'applied once in memory' not in result.stderr
        cases.append('default off; no template required; unrelated overlay preserved')
        for case, scale, mmap, split in [('floor-075', '0.75', False, False), ('floor-070', '0.70', False, False),
                                         ('floor-060', '0.60', False, False), ('near-native', '0.7999999', False, False),
                                         ('mmap', '0.75', True, False), ('split', '0.75', False, True)]:
            result, output, source_template = run(case, scale=scale, mmap=mmap, split=split)
            assert result.returncode == 0, result.stderr
            assert result.stderr.count('applied once in memory') == 1
            frame = output.read_bytes()
            assert decoder.lib.ZSTD_getDictID_fromFrame(frame, len(frame)) == 3
            decoded = decoder.decode(frame, dictionary)
            assert len(decoded) == len(original) == drs.TEMPLATE_SIZE
            assert struct.unpack_from('<I', decoded, floor)[0] == struct.unpack('<I', struct.pack('<f', float(scale)))[0]
            assert decoded[:floor] == original[:floor] and decoded[floor + 4:] == original[floor + 4:]
            after = drs.sarc_members(decoded)
            assert after == members and len(after) == 292
            for name, (lo, hi) in members.items():
                if name != drs.MEMBER:
                    assert decoded[lo:hi] == original[lo:hi], name
            assert source_template.read_bytes() == original, 'runtime must never modify the source template'
            cases.append(case + ': open/reopen/size/partial reads/EOF; libzstd dict3 roundtrip; all 292 members/accounting preserved')
        result, output, source_template = run('custom-template', custom_template=True)
        assert result.returncode == 0, result.stderr
        decoded = decoder.decode(output.read_bytes(), dictionary)
        assert decoded[:floor] == original[:floor] and decoded[floor + 4:] == original[floor + 4:]
        assert struct.unpack_from('<f', decoded, floor)[0] == 0.75 and source_template.read_bytes() == original
        cases.append('explicit TOTK_DRS_TEMPLATE path selected and authenticated')
        bad_source = bytes([packed[0] ^ 1]) + packed[1:]
        bad_template = bytes([original[0] ^ 1]) + original[1:]
        bad_member = bytearray(original)
        bad_member[floor] ^= 1
        refusals = [
            ('missing-template', dict(template_bytes=None), 'template missing'),
            ('wrong-template-size', dict(template_bytes=original[:-1]), '1795492-byte'),
            ('wrong-template-sha', dict(template_bytes=bad_template), 'template SHA-256 mismatch'),
            ('changed-member', dict(template_bytes=bytes(bad_member)), 'template SHA-256 mismatch'),
            ('wrong-original-sha', dict(source=bad_source), 'overlay integrity mismatch'),
            ('bootup-conflict', dict(bootup_mod=True), 'DRS refuses a different Bootup overlay'),
            ('wrong-version', dict(old_version=True, template_bytes=None), 'requires the authenticated 1.4.3'),
        ]
        for case, kwargs, message in refusals:
            result, output, _ = run(case, **kwargs)
            assert result.returncode == 7 and message in result.stderr and not output.exists(), result.stderr
            cases.append(case + ': refused before serving')
        invalid = ['', '0', '-0.1', '0.8', '0.800000011920929', '0.79999999999', '1', 'nan', 'inf', '-inf',
                   'garbage', '0.75oops', ' 0.75', '0.75 ', '1e-1000', '1e-46']
        for i, scale in enumerate(invalid):
            result, output, _ = run(f'invalid-{i}', scale=scale, template_bytes=None)
            assert result.returncode == 7 and 'must be finite, positive' in result.stderr and not output.exists(), (scale, result.stderr)
        cases.append(f'{len(invalid)} invalid scales rejected, including float rounding/underflow')
        for size in [0, 1, 131071, 131072, 131073, 262144]:
            data = bytes((i * 73 + 19) & 255 for i in range(size))
            source = root / f'raw-{size}'
            output = root / f'frame-{size}'
            source.write_bytes(data)
            subprocess.run([str(fixture), 'frame', str(source), str(output)], check=True)
            frame = output.read_bytes()
            assert decoder.lib.ZSTD_getDictID_fromFrame(frame, len(frame)) == 3
            assert decoder.decode(frame, dictionary) == data
        cases.append('independent raw-zstd decoder checks at empty/exact/multiple 128-KiB block boundaries')
    print(json.dumps(dict(status='pass', cases=cases, source_pack_sha256=drs.PACK_SHA,
                          template_sha256=drs.TEMPLATE_SHA, members=292,
                          runtime_game_execution='not exercised'), indent=2))


if __name__ == '__main__':
    main()
