"""The game's words at the v143 patch sites, from the player's own main NSO, for the verify scripts and their C fixtures.

The repository holds no game code: each script names its guarded offsets and the SHA-256 of the words there
(runtime/code_guard.h hashes the same way), reads the words from --main-nso, checks them by hash and hands them to its
fixture (scripts/fixture_seeds.h) in a seed file. standin() mirrors code_guard_standin() for dataless builds."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess

MAIN_NSO_SHA256 = 'e84a90d551fed0824b4b74933b9c6d043c458521363a7cea58b2faca1822dc9e'
GAME = Path(__file__).resolve().parents[1]


def build(source, output, standin=False):
    """Compile a v143 fixture with the production patches; standin: dataless CODE_GUARD_STANDIN build."""
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-Wno-misleading-indentation', '-Wno-format',
                    *(['-DCODE_GUARD_STANDIN=1'] if standin else []), '-I' + str(GAME / 'runtime'),
                    str(source), str(GAME / 'android/gfx/host_test_support.c'), '-lm', '-o', str(output)], check=True)
    return output


def run(fixture, args, seeds=None):
    """Run a fixture with the seed file (None for a stand-in build)."""
    env = dict(os.environ, FIXTURE_SEEDS=str(seeds)) if seeds else os.environ
    return subprocess.run([str(fixture), *map(str, args)], check=True, capture_output=True, text=True, env=env)
MAIN_NSO_SHA256 = 'e84a90d551fed0824b4b74933b9c6d043c458521363a7cea58b2faca1822dc9e'


def read_main(path):
    """The 1.4.3 main NSO's segments: original(address, size) -> bytes."""
    import lz4.block
    raw = path.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == MAIN_NSO_SHA256, f'{path} is not the 1.4.3 main NSO'
    assert raw[:4] == b'NSO0'
    segments = []
    for i in range(3):
        offset, address, size = struct.unpack_from('<III', raw, 0x10 + 16 * i)
        compressed = struct.unpack_from('<I', raw, 0x60 + 4 * i)[0]
        data = lz4.block.decompress(raw[offset:offset + compressed], uncompressed_size=size) if raw[0xc] & (1 << i) else raw[offset:offset + size]
        assert len(data) == size and hashlib.sha256(data).digest() == raw[0xa0 + i * 32:0xc0 + i * 32]
        segments.append((address, data))

    def original(address, size):
        for start, data in segments:
            if start <= address and address + size <= start + len(data):
                return data[address - start:address - start + size]
        raise AssertionError(hex(address))
    return original


def words(original, offsets):
    return [struct.unpack('<I', original(off, 4))[0] for off in offsets]


def digest(values):
    return hashlib.sha256(b''.join(struct.pack('<I', w) for w in values)).hexdigest()


def check(original, offsets, sha256):
    """The words at offsets, after checking their SHA-256 (as the runtime's code_guard does)."""
    found = words(original, offsets)
    assert digest(found) == sha256, f'main words at {[hex(o) for o in offsets]} are not the expected ones'
    return found


def write_seeds(path, original, offsets):
    """The seed file for fixture_seeds.h: <u64 offset><u32 word> per site."""
    path.write_bytes(b''.join(struct.pack('<QI', off, struct.unpack('<I', original(off, 4))[0]) for off in offsets))
    return path


def standin(off):
    """code_guard_standin(): the word a CODE_GUARD_STANDIN build expects at off."""
    mask = (1 << 64) - 1
    z = (off + 0x9e3779b97f4a7c15) & mask
    z = ((z ^ (z >> 30)) * 0xbf58476d1ce4e5b9) & mask
    z = ((z ^ (z >> 27)) * 0x94d049bb133111eb) & mask
    return (z ^ (z >> 31)) & 0xffffffff
