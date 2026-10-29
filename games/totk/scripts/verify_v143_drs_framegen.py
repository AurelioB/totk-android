#!/usr/bin/env python3
"""Authenticate original v143 sites and emulate the production mutable DRS veneers.

No game/device/GPU execution. Requires lz4 and Unicorn; all outputs under --scratch.
"""
import argparse
import json
from pathlib import Path
import struct
import fixture_seeds
import verify_v143_drs60 as native
from unicorn import arm64_const as reg

ROOT = Path(__file__).resolve().parents[1]
DATA, SITE, LEVEL = 0x2d04000, 0x584e84, 0x5848a8
# The level-sample sites beyond DRS60's; SHA-256 of their words (runtime/code_guard.h).
EXTRA_SITES = [LEVEL, 0x5848ac, 0x5848b0]
EXTRA_SHA256 = '117cf2ede10abc80e24e1fed657758b4ab615076f62a77df3a9d3d9d37f838ae'


def compile_fixture(scratch, original):
    """The fixture seeded with the player's words (original: native.read_original of their main NSO)."""
    fixture_seeds.check(original, EXTRA_SITES, EXTRA_SHA256)
    seeds = fixture_seeds.write_seeds(scratch / 'seeds.bin', original, native.GUARD_SITES + EXTRA_SITES)
    fixture = fixture_seeds.build(ROOT / 'scripts/v143_drs_framegen_fixture.c', scratch / 'drs-framegen-fixture')
    image, diag = scratch / 'budget.bin', scratch / 'diag.bin'
    run = fixture_seeds.run(fixture, [image, diag], seeds)
    (scratch / 'fixture.log').write_text(run.stdout + run.stderr)
    return image.read_bytes(), diag.read_bytes(), run.stdout.strip()


def operand_checks(image):
    cases = []
    registers = [getattr(reg, f'UC_ARM64_REG_X{i}') for i in range(31)]
    vectors = [getattr(reg, f'UC_ARM64_REG_Q{i}') for i in range(32)]
    for cap in [144.0, 153.0, 180.0, 3.4028234663852886e38]:
        for budget in [80.0, 153.0, 185.0, 200.0, 205.0, 1000.0]:
            mu = native.machine(image)
            for i, r in enumerate(registers):
                mu.reg_write(r, 0xabcd0000 + i)
            mu.reg_write(reg.UC_ARM64_REG_X8, 0x10000000)
            initial = [mu.reg_read(r) for r in registers]
            for i, r in enumerate(vectors):
                mu.reg_write(r, 0x123456789abcdef00123456789abcdef0 + i)
            initial_vectors = [mu.reg_read(r) for r in vectors]
            mu.reg_write(reg.UC_ARM64_REG_NZCV, 0xa0000000)
            mu.reg_write(reg.UC_ARM64_REG_SP, 0x1000f000)
            mu.mem_write(DATA + 48, struct.pack('<f', cap))
            mu.mem_write(0x1000000c, struct.pack('<f', budget))
            mu.emu_start(SITE, SITE + 4, count=16)
            assert mu.reg_read(reg.UC_ARM64_REG_PC) == SITE + 4
            assert mu.reg_read(reg.UC_ARM64_REG_S17) == native.bits(min(budget, cap))
            assert [mu.reg_read(r) for r in registers] == initial
            assert [mu.reg_read(r) for i, r in enumerate(vectors) if i not in (17, 22)] == [v for i, v in enumerate(initial_vectors) if i not in (17, 22)]
            assert mu.reg_read(reg.UC_ARM64_REG_NZCV) == 0xa0000000
            assert mu.reg_read(reg.UC_ARM64_REG_SP) == 0x1000f000
            assert struct.unpack('<f', mu.mem_read(0x1000000c, 4))[0] == budget
            cases.append([budget, cap, min(budget, cap)])
    return cases


def level_checks(image):
    cases = 0
    for aspect in [0.0, 7.0 / 3.0]:
        for priority, scale in [(-1, 1.0), (0, .9), (1, .8), (1, .75)]:
            mu = native.machine(image)
            registers = [getattr(reg, f'UC_ARM64_REG_X{i}') for i in range(31)]
            vectors = [getattr(reg, f'UC_ARM64_REG_Q{i}') for i in range(32)]
            for i, r in enumerate(registers):
                mu.reg_write(r, 0xabcd0000 + i)
            mu.reg_write(reg.UC_ARM64_REG_X19, 0x10000000)
            mu.reg_write(reg.UC_ARM64_REG_W23, native.bits(scale))
            initial = [mu.reg_read(r) for r in registers]
            initial_vectors = [mu.reg_read(r) for r in vectors]
            mu.reg_write(reg.UC_ARM64_REG_NZCV, 0xa0000000)
            mu.reg_write(reg.UC_ARM64_REG_SP, 0x1000f000)
            mu.mem_write(0x10000134, struct.pack('<i', priority))
            aspect_data = struct.pack('<fI', aspect, 0xcafebabe)
            mu.mem_write(DATA + 56, aspect_data)
            mu.emu_start(LEVEL, LEVEL + 4, count=16)
            assert mu.reg_read(reg.UC_ARM64_REG_PC) == LEVEL + 4
            assert mu.mem_read(DATA + 64, 8) == struct.pack('<Ii', native.bits(scale), priority)
            assert mu.mem_read(DATA + 56, 8) == aspect_data, 'DRS snapshot corrupted live camera aspect'
            assert mu.reg_read(reg.UC_ARM64_REG_W9) == native.bits(scale)
            assert [mu.reg_read(r) for i, r in enumerate(registers) if i != 9] == [v for i, v in enumerate(initial) if i != 9]
            assert [mu.reg_read(r) for r in vectors] == initial_vectors
            assert mu.reg_read(reg.UC_ARM64_REG_NZCV) == 0xa0000000
            assert mu.reg_read(reg.UC_ARM64_REG_SP) == 0x1000f000
            # Execute the production aspect veneer installed by the same C
            # fixture. A real DRS return must not enable/change its override.
            mu.reg_write(reg.UC_ARM64_REG_S1, native.bits(16.0))
            mu.reg_write(reg.UC_ARM64_REG_S2, native.bits(9.0))
            mu.emu_start(0x1000, 0x1004, count=16)
            assert mu.reg_read(reg.UC_ARM64_REG_PC) == 0x1004
            assert mu.reg_read(reg.UC_ARM64_REG_S0) == native.bits(aspect or 16.0 / 9.0)
            cases += 1
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--main-nso', required=True, type=Path)
    parser.add_argument('--scratch', required=True, type=Path)
    args = parser.parse_args()
    scratch = args.scratch.resolve()
    assert scratch != ROOT and ROOT not in scratch.parents
    scratch.mkdir(parents=True, exist_ok=True)
    original = native.read_original(args.main_nso)
    image, diag, guards = compile_fixture(scratch, original)
    operands = operand_checks(image)
    levels = level_checks(diag)
    decisions = []
    for cap in [144.0, 153.0, 180.0, 3.4028234663852886e38]:
        code = bytearray(image)
        struct.pack_into('<f', code, DATA + 48, cap)
        for budget in [80.0, 185.0, 200.0, 205.0]:
            for stress in [40.0, 100.0, 150.0, 200.0, 300.0]:
                baseline = native.native_decision(bytes(code), original, budget, stress, False)
                retarget = native.native_decision(bytes(code), original, budget, stress, True)
                assert baseline['predicted_stress'] == retarget['predicted_stress']
                assert retarget['level'] >= baseline['level']
                if budget <= cap:
                    assert baseline == retarget, 'inactive or tighter native budget changed'
                decisions.append(dict(cap=cap, budget=budget, stress=stress, baseline=baseline, framegen=retarget))
    assert any(row['baseline']['level'] == -1 and row['framegen']['level'] >= 0 for row in decisions)
    report = dict(status='pass', guards=guards, operand_cases=operands, level_cases=levels,
                  native_decisions=decisions, scope='authenticated sites, production veneers and native predictor; synthetic inputs only')
    (scratch / 'checks.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
