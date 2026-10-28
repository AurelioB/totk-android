#!/usr/bin/env python3
"""Host-only production renderer patch guards and ARM64 veneer semantics.

Python needs unicorn and lz4; --output-dir keeps all generated files out of build/.
--main-nso (the user's 1.4.3 main) supplies the site words: their SHA-256 is checked and the fixture and the
emulation use them (the repository holds no game code).
No game entry point, device or GPU is used; no performance/visual claim is made.
"""
import argparse
import json
from pathlib import Path
import struct

import fixture_seeds
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn import arm64_const as regs

ROOT = Path(__file__).resolve().parents[1]
# Shadow-resolution (16), far-clamp (6) and FXAA (1) sites; SHA-256 of their words (runtime/code_guard.h).
SITES = [0x6835a4, 0x6835a8, 0x683604, 0x683608, 0x68360c, 0x683610, 0x683614, 0x683618,
         0x68361c, 0x683620, 0x683624, 0x683628, 0x68362c, 0x683630, 0x683634, 0x683638,
         0x628c20, 0x628c28, 0x628c30, 0x628c34, 0x628c38, 0x628c3c, 0x586944]
SITES_SHA256 = 'fa28a98dc84231f3cbf8cdccdedfbb9c2f934e306a8ea496124a08e5bc4e5fce'


def check_original(original, text_end):
    fixture_seeds.check(original, SITES, SITES_SHA256)
    stub = ((text_end + 15) & ~15) + 288
    assert stub + 32 <= (text_end + 0xfff) & ~0xfff
    return {'guards': len(SITES), 'text_end': hex(text_end), 'far_stub': hex(stub)}


def text_end(path):
    raw = path.read_bytes()
    _, addr, size = struct.unpack_from('<III', raw, 0x10)
    return addr + size


def machine(image):
    mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    mu.mem_map(0, len(image))
    mu.mem_write(0, image)
    mu.mem_map(0x10000000, 0x10000)
    return mu


def fbits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def far_cases(image):
    cases = []
    for far in [3000.0, 12000.0, 25000.0]:
        mu = machine(image)
        source, camera, stack = 0x10000000, 0x10001000, 0x1000f000
        gprs = [getattr(regs, f'UC_ARM64_REG_X{i}') for i in range(29)] + [regs.UC_ARM64_REG_X29, regs.UC_ARM64_REG_X30]
        for i, r in enumerate(gprs):
            mu.reg_write(r, 0xabcd0000 + i)
        mu.reg_write(regs.UC_ARM64_REG_X19, source)
        mu.reg_write(regs.UC_ARM64_REG_X20, camera)
        before = [mu.reg_read(r) for r in gprs]
        simd = [getattr(regs, f'UC_ARM64_REG_Q{i}') for i in range(32)]
        for i, r in enumerate(simd):
            mu.reg_write(r, 0x123456789abcdef00123456789abcdef0 + i)
        before_simd = [mu.reg_read(r) for r in simd]
        mu.reg_write(regs.UC_ARM64_REG_SP, stack)
        mu.reg_write(regs.UC_ARM64_REG_NZCV, 0xa0000000)
        mu.mem_write(source + 0x134, struct.pack('<fff', 0.25, far, 50.0))
        mu.mem_write(camera + 0x98, struct.pack('<fff', 0.25, -1.0, 50.0))
        mu.emu_start(0x628c30, 0x628c3c, count=16)
        assert mu.reg_read(regs.UC_ARM64_REG_PC) == 0x628c3c
        expected = min(far, 12000.0)
        assert mu.reg_read(regs.UC_ARM64_REG_S0) == fbits(expected)
        assert struct.unpack('<fff', mu.mem_read(source + 0x134, 12)) == (0.25, expected, 50.0)
        assert struct.unpack('<fff', mu.mem_read(camera + 0x98, 12)) == (0.25, expected, 50.0)
        assert [mu.reg_read(r) for r in gprs] == before, 'GPR/LR clobber'
        assert [mu.reg_read(r) for r in simd[1:]] == before_simd[1:], 'live SIMD clobber'
        assert mu.reg_read(regs.UC_ARM64_REG_SP) == stack
        assert mu.reg_read(regs.UC_ARM64_REG_NZCV) == 0xa0000000
        cases.append([far, expected])
    return cases


def shadow_cases(image):
    cases = []
    for n in [288, 256]:
        for quality in [0, 1 << 7]:
            mu = machine(image)
            # Both encodings asserted against production output by the C fixture.
            mu.mem_write(0x6835a4, struct.pack('<I', 0x5280000b | (n << 5)))
            source, sizes = 0x10000000, 0x10002000
            mu.reg_write(regs.UC_ARM64_REG_X0, source)
            mu.reg_write(regs.UC_ARM64_REG_X12, sizes)
            mu.reg_write(regs.UC_ARM64_REG_X13, 0)
            mu.reg_write(regs.UC_ARM64_REG_X15, 4)  # existing cascade count, not patched
            mu.reg_write(regs.UC_ARM64_REG_X14, quality)
            mu.emu_start(0x6835a4, 0x68363c, count=24)
            assert mu.reg_read(regs.UC_ARM64_REG_PC) == 0x68363c
            assert mu.reg_read(regs.UC_ARM64_REG_X19) == n
            assert struct.unpack('<II', mu.mem_read(source + 0xf4, 8)) == (n, n)
            assert struct.unpack('<I', mu.mem_read(sizes + 0x3d8, 4))[0] == n
            assert struct.unpack('<I', mu.mem_read(sizes + 0x3f8, 4))[0] == n
            assert struct.unpack('<I', mu.mem_read(sizes + 0x4d8, 4))[0] == 4
            cases.append([n, quality])
    return cases


def fxaa_case(image):
    mu = machine(image)
    mu.reg_write(regs.UC_ARM64_REG_X0, 0x10000000)
    mu.reg_write(regs.UC_ARM64_REG_X9, 1)
    mu.mem_write(0x1000018b, b'\xaa\xff\xbb')
    mu.emu_start(0x586944, 0x586948, count=1)
    assert bytes(mu.mem_read(0x1000018b, 3)) == b'\xaa\x00\xbb'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--main-nso', required=True, type=Path)
    args = parser.parse_args()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    original = fixture_seeds.read_main(args.main_nso)
    nso = check_original(original, text_end(args.main_nso))
    seeds = fixture_seeds.write_seeds(out / 'render-seeds.bin', original, SITES)
    binary, dump = out / 'v143-render-fixture', out / 'render.bin'
    fixture_seeds.build(ROOT / 'scripts/v143_render_fixture.c', binary)
    run = fixture_seeds.run(binary, [dump], seeds)
    image = dump.read_bytes()
    result = dict(status='pass', guards=run.stdout.strip(), far=far_cases(image),
                  shadow=shadow_cases(image), fxaa_byte_store=True, original_nso=nso,
                  scope='production patch guards and ARM64 semantics; no GPU/performance/device claim')
    fxaa_case(image)
    (out / 'render-verification.json').write_text(json.dumps(result, indent=2) + '\n')
    (out / 'render-fixture.log').write_text(run.stdout + run.stderr)
    print(json.dumps(result))


if __name__ == '__main__':
    main()
