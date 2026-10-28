#!/usr/bin/env python3
"""Authenticate v143 bytes, test production DRS60 guards and emulate native decisions.

Requires Python lz4 and unicorn. Outputs only under --scratch; no guest entry,
GPU, Android build or device is exercised. Use --main-nso from the player's files.
"""
import argparse
import fixture_seeds
import json
from pathlib import Path
import struct

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn import arm64_const as reg

ROOT = Path(__file__).resolve().parents[1]
SITE, STUB = 0x584e84, 0x690240
# The production DRS60 sites and the time-unit constant; SHA-256 of their words (runtime/code_guard.h).
GUARD_SITES = [SITE, 0x584e88, 0x584e8c, 0x584e90, 0x584790, 0x584b68, 0xa1bb0, 0xa1bb4, 0x2d02094]
GUARD_SHA256 = '43445c25d58e199ca9133dd996919011b6cee5351bd98d8b5831588b43dae824'


def read_original(path):
    original = fixture_seeds.read_main(path)
    fixture_seeds.check(original, GUARD_SITES, GUARD_SHA256)
    assert struct.unpack('<f', original(0x2d02608, 4))[0] == struct.unpack('<f', struct.pack('<f', .001))[0]
    return original


def bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def machine(image):
    mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    mu.mem_map(0, len(image))
    mu.mem_write(0, image)
    mu.mem_map(0x10000000, 0x10000)
    return mu


def operand_checks(image):
    cases = []
    for budget in [80.0, 100.0, 185.0, 205.0, 1000.0]:
        mu = machine(image)
        registers = [getattr(reg, f'UC_ARM64_REG_X{i}') for i in range(29)] + [reg.UC_ARM64_REG_X29, reg.UC_ARM64_REG_X30]
        for i, r in enumerate(registers):
            mu.reg_write(r, 0xabcd0000 + i)
        mu.reg_write(reg.UC_ARM64_REG_X8, 0x10000000)
        initial = [mu.reg_read(r) for r in registers]
        vectors = [getattr(reg, f'UC_ARM64_REG_Q{i}') for i in range(32)]
        for i, r in enumerate(vectors):
            mu.reg_write(r, 0x123456789abcdef00123456789abcdef0 + i)
        initial_vectors = [mu.reg_read(r) for r in vectors]
        mu.reg_write(reg.UC_ARM64_REG_NZCV, 0xa0000000)
        mu.reg_write(reg.UC_ARM64_REG_SP, 0x1000f000)
        mu.mem_write(0x1000000c, struct.pack('<f', budget))
        mu.emu_start(SITE, SITE + 4, count=8)
        assert mu.reg_read(reg.UC_ARM64_REG_PC) == SITE + 4
        assert mu.reg_read(reg.UC_ARM64_REG_S17) == bits(min(budget, 100.0))
        assert [mu.reg_read(r) for r in registers] == initial
        assert [mu.reg_read(r) for i, r in enumerate(vectors) if i not in (17, 22)] == [v for i, v in enumerate(initial_vectors) if i not in (17, 22)]
        assert mu.reg_read(reg.UC_ARM64_REG_NZCV) == 0xa0000000
        assert mu.reg_read(reg.UC_ARM64_REG_SP) == 0x1000f000
        assert struct.unpack('<f', mu.mem_read(0x1000000c, 4))[0] == budget, 'native manager budget mutated'
        cases.append([budget, min(budget, 100.0)])
    return cases


def native_decision(image, original, budget, stress, patched):
    code = bytearray(image)
    code[0x584d70:0x584fd0] = original(0x584d70, 0x260)
    code[0x2d01000:0x2d03000] = original(0x2d01000, 0x2000)
    if patched:
        code[SITE:SITE + 4] = image[SITE:SITE + 4]
    mu = machine(bytes(code))
    mu.mem_map(0x3ada000, 0x1000)
    manager, first, second, system, stop = 0x10000000, 0x10001000, 0x10002000, 0x10004000, 0x10008000
    mu.mem_write(0x3ada848, struct.pack('<Q', system))
    mu.mem_write(system + 0x10, struct.pack('<f', 1.0))
    mu.mem_write(manager + 0xc, struct.pack('<f', budget))
    mu.mem_write(manager + 0x6c, struct.pack('<I', 1280))
    mu.mem_write(manager + 0x83, b'\x01')
    mu.mem_write(manager + 0x18d, b'\x01')
    mu.mem_write(manager + 0x160, struct.pack('<I', 2))
    mu.mem_write(manager + 0x158, struct.pack('<Q', first + 0x18))
    for address, scale, priority, previous in [(first, .9, 0, second + 0x18), (second, .8, 1, manager + 0x150)]:
        mu.mem_write(address + 4, struct.pack('<f', scale))
        mu.mem_write(address + 8, b'\x01')
        mu.mem_write(address + 0x10, struct.pack('<I', priority))
        mu.mem_write(address + 0x20, struct.pack('<Q', previous))
    mu.reg_write(reg.UC_ARM64_REG_X0, manager)
    mu.reg_write(reg.UC_ARM64_REG_X30, stop)
    mu.reg_write(reg.UC_ARM64_REG_SP, 0x1000f000)
    mu.reg_write(reg.UC_ARM64_REG_S0, bits(stress))
    mu.emu_start(0x584d70, stop, count=500)
    assert mu.reg_read(reg.UC_ARM64_REG_PC) == stop
    value = mu.reg_read(reg.UC_ARM64_REG_W0)
    return {'level': -1 if value == 0xffffffff else value,
            'predicted_stress': struct.unpack('<f', mu.mem_read(manager + 0x10, 4))[0]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--main-nso', required=True, type=Path)
    parser.add_argument('--scratch', required=True, type=Path)
    args = parser.parse_args()
    scratch = args.scratch.resolve()
    assert scratch != ROOT and ROOT not in scratch.parents, 'scratch must be outside source game directory'
    scratch.mkdir(parents=True, exist_ok=True)
    original = read_original(args.main_nso)
    fixture, dump = scratch / 'drs60-fixture', scratch / 'drs60-fixture.bin'
    seeds = fixture_seeds.write_seeds(scratch / 'drs60-seeds.bin', original, GUARD_SITES)
    fixture_seeds.build(ROOT / 'scripts/v143_drs60_fixture.c', fixture)
    run = fixture_seeds.run(fixture, [dump], seeds)
    image = dump.read_bytes()
    operands = operand_checks(image)
    decisions = []
    for budget in [80.0, 100.0, 185.0, 205.0]:
        for stress in [40.0, 100.0, 150.0, 200.0, 300.0]:
            baseline = native_decision(image, original, budget, stress, False)
            retarget = native_decision(image, original, budget, stress, True)
            assert baseline['predicted_stress'] == retarget['predicted_stress'], 'measurement/cost reporting changed'
            assert retarget['level'] >= baseline['level'], 'retarget chose a higher-cost level'
            if budget <= 100:
                assert baseline == retarget, 'already tighter native budget changed'
            decisions.append(dict(budget=budget, stress=stress, baseline=baseline, drs60=retarget))
    assert any(row['baseline']['level'] == -1 and row['drs60']['level'] >= 0 for row in decisions), 'no demonstrated earlier reduction'
    report = dict(status='pass', original_nso_guards=len(GUARD_SITES), fixture=run.stdout.strip(),
                  budget_operands=operands, native_decisions=decisions,
                  scope='production patch plus original v143 predictor; synthetic controller inputs, no game/GPU/device execution')
    (scratch / 'drs60-checks.json').write_text(json.dumps(report, indent=2) + '\n')
    (scratch / 'drs60-fixture.log').write_text(run.stdout + run.stderr)
    print(json.dumps(report))


if __name__ == '__main__':
    main()
