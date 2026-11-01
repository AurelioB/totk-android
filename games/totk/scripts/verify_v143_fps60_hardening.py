#!/usr/bin/env python3
"""Check real v143 words, production patch guards, ARM veneers and HLE pacing.

The image and optional source NSO are caller-owned. Only --out is written; no
full guest entry, game services, GPU or device is executed. Unicorn emulates the
emitted veneers, with only CNTVCT reads replaced by a deterministic counter.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as a

ROOT = Path(__file__).resolve().parents[1]
# The production sites and predicate guards; SHA-256 of their words in this order (runtime/code_guard.h).
SITES = (0x0a7ac4, 0x95dd24, 0x6155e4, 0x615660, 0x45c700, 0x2b23c7c, 0x42c3bc, 0xa5a188, 0x2db238, 0xa5a670)
SITES_SHA256 = '41d0c2905dd9466e7ea7dcc0885beedc9ea0c48d5cce30ea543a0b1fdfb2ab43'
PREDICATE_GUARDS = (0xa5a18c, 0xa5a660, 0xa5a664, 0xa5a668, 0xa5a66c, 0xa5a674)
PREDICATES_SHA256 = 'edba8d506fed22b2f18d5ec5cf90dbbc673c041efaa7e1ccb0f3e0ee488d6300'
FADES = SITES[:4]
MARKERS = SITES[4:8]
X = [getattr(a, f'UC_ARM64_REG_X{i}') for i in range(31)]
Q = [getattr(a, f'UC_ARM64_REG_Q{i}') for i in range(32)]


def function(source, start):
    if source.count(start) != 1:
        raise ValueError(f'production function anchor changed: {start!r}')
    begin = source.index(start)
    return source[begin:source.index('\n}', begin) + 2] + '\n'


def check_words(read):
    """read(offset) -> 4 bytes; both guarded groups must hash as the runtime expects."""
    for sites, sha in ((SITES, SITES_SHA256), (PREDICATE_GUARDS, PREDICATES_SHA256)):
        assert hashlib.sha256(b''.join(read(site) for site in sites)).hexdigest() == sha, 'v143 words differ'


def source_words(path):
    import lz4.block
    raw = path.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    assert digest == 'e84a90d551fed0824b4b74933b9c6d043c458521363a7cea58b2faca1822dc9e'
    offset, base, size = struct.unpack_from('<III', raw, 0x10)
    compressed, = struct.unpack_from('<I', raw, 0x60)
    text = lz4.block.decompress(raw[offset:offset + compressed], uncompressed_size=size) if raw[0xc] & 1 else raw[offset:offset + size]
    assert hashlib.sha256(text).digest() == raw[0xa0:0xc0]
    check_words(lambda site: text[site - base:site - base + 4])
    return digest


class Guest:
    def __init__(self, image):
        self.mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.mu.mem_map(0, (len(image) + 0xfff) & ~0xfff)
        self.mu.mem_write(0, image)
        self.arena = 0x10000000
        self.mu.mem_map(self.arena, 0x20000)
        self.clock = 1000000000
        def counter(uc, pc, size, _):
            word, = struct.unpack('<I', uc.mem_read(pc, 4))
            if word & ~31 == 0xd53be040:  # mrs xD,cntvct_el0; replace ONLY the clock source
                uc.reg_write(X[word & 31], self.clock)
                uc.reg_write(a.UC_ARM64_REG_PC, pc + 4)
        self.mu.hook_add(UC_HOOK_CODE, counter)

    def execute(self, site, bits=0, candidate=1, camera_active=True, camera_complete=False):
        uc = self.mu
        for i, reg in enumerate(X):
            uc.reg_write(reg, 0x1234000000000000 + i)
        uc.reg_write(X[0], self.arena + 0x100)
        uc.mem_write(self.arena + 0x120, struct.pack('<Q', 0xabcdef0123456789 if camera_active else 0))
        uc.mem_write(self.arena + 0x118, struct.pack('<I', 0x80000000 if camera_complete else 0))
        if site == 0xa5a670:
            uc.reg_write(X[19], self.arena + 0x200)
            uc.reg_write(X[10], 0x80000000)  # the actual completion-store value
        uc.reg_write(X[21], candidate)
        for i, reg in enumerate(Q):
            uc.reg_write(reg, (0xabcdef0123456789 << 64) | (0xfedcba98 << 32) | (bits if i == 0 else i))
        uc.reg_write(a.UC_ARM64_REG_SP, self.arena + 0x10000)
        uc.reg_write(a.UC_ARM64_REG_NZCV, 0xa0000000)
        uc.reg_write(a.UC_ARM64_REG_FPCR, 0)
        uc.reg_write(a.UC_ARM64_REG_FPSR, 0x10)
        uc.emu_start(site, site + 4, count=100)
        assert uc.reg_read(a.UC_ARM64_REG_PC) == site + 4
        regs = X + Q + [a.UC_ARM64_REG_SP, a.UC_ARM64_REG_NZCV,
                        a.UC_ARM64_REG_FPCR, a.UC_ARM64_REG_FPSR]
        return {reg: uc.reg_read(reg) for reg in regs}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--nso', type=Path, help='also authenticate/decompress the original v143 main NSO')
    parser.add_argument('--text-end', type=lambda s: int(s, 0), required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--cc', default='clang')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    raw = args.image.read_bytes()
    check_words(lambda site: raw[site:site + 4])
    nso_digest = source_words(args.nso) if args.nso else None
    stub = ((args.text_end + 15) & ~15) + 416
    data = (len(raw) + 0x3fff) & ~0x3fff
    code_size = (4 * 10 + 3 * 8 + 26 + 10 + 25) * 4
    nvn = (ROOT / 'third_party/d2r_gfx/gfx/nvn/hle_nvn.c').read_text()
    (out / 'fps60_hardening_present.inc').write_text(''.join(function(nvn, anchor) for anchor in (
        'static unsigned cutscene_present_ticks(unsigned ticks) {',
        'static unsigned present_ticks(const win_t *o) {',
        'static uint64_t present_period(const win_t *o) {')))
    compat = ROOT / 'android/compat/include'
    fixture = out / 'fps60-hardening-fixture'
    subprocess.run([args.cc, '-std=gnu11', '-O1', '-D_GNU_SOURCE', '-DTOTK_GAME_V143=1',
        '-I' + str(compat), '-include', str(compat / 'darwin_compat.h'),
        '-I' + str(ROOT / 'third_party/d2r_gfx/host'), '-I' + str(ROOT / 'runtime'),
        '-I' + str(out), str(ROOT / 'scripts/v143_fps60_hardening_fixture.c'),
        str(ROOT / 'android/gfx/host_test_support.c'), '-lm', '-o', str(fixture)], check=True)
    # Avoid accidentally inheriting optional patches or a device/operator profile.
    env = {key: value for key, value in os.environ.items() if not key.startswith(('TOTK_', 'DM_'))}
    env |= {'TOTK_REALTIME': '0', 'TOTK_OUTPUT_RES': '0', 'TOTK_ASPECT': '0',
            'TOTK_RENDER_RES': '0', 'TOTK_NATIVE_UI': '0', 'TOTK_HIGHFPS_FIX': '0',
            'TOTK_DRS60': '0', 'TOTK_DRS_FRAMEGEN': '0'}
    cases = [('native60', {'TOTK_FPS60': '1', 'TOTK_FRAMEGEN': '0'}, True, True),
             ('desktop-default60', {}, True, True),
             ('android-default30', {'TOTK_FPS60': '0'}, False, False),
             ('default60-floor-opt-out', {'TOTK_FPS60_HARDENING': '0'}, True, False)]
    # All explicit FPS60/framegen/hardening combinations, plus unset/empty
    # framegen: only FPS60 may gate fades; the floor keeps its original policy.
    for mode in (None, '', '0', 'locked', 'interp', 'hybrid', 'adaptive', '1'):
        for fps in ('0', '1'):
            for hardening in ('0', '1'):
                settings = {'TOTK_FPS60': fps, 'TOTK_FPS60_HARDENING': hardening}
                if mode is not None:
                    settings['TOTK_FRAMEGEN'] = mode
                fades = fps != '0'
                floor = fades and mode in (None, '', '0') and hardening != '0'
                label = 'unset' if mode is None else mode or 'empty'
                cases.append((f'gate-fps{fps}-framegen-{label}-hardening{hardening}',
                              settings, fades, floor))
    patched = None
    checked_modes = []
    for name, settings, fades, floor in cases:
        path = out / (name + '.bin')
        run = subprocess.run([str(fixture), str(args.image), str(path), hex(args.text_end)],
            env=env | settings, capture_output=True, text=True, check=True)
        (out / (name + '.log')).write_text(run.stdout + run.stderr)
        result = json.loads(run.stdout)
        assert result['fade_clamps'] == fades, (name, result)
        assert result['hardening'] == floor, (name, result)
        if floor:
            for text in ('admission=camera-active', 'admission=camera-completed', 'admission=camera-empty',
                         'decision=expired', 'decision=no-authority', '0@0x45c700(reset)',
                         '1@0x2b23c7c(type-query)', '2@0x42c3bc(unclassified-update)',
                         '3@0xa5a188(scripted-camera)'):
                assert text in run.stderr, (name, text)
        image = path.read_bytes()
        if name == 'native60':
            patched = image
            assert all(struct.unpack_from('<I', image, site)[0] & 0xfc000000 == 0x14000000 for site in SITES)
        assert patched is not None
        for site in SITES:
            active = fades and (site in FADES or floor)
            expected = patched if active else raw
            assert image[site:site + 4] == expected[site:site + 4], (name, hex(site))
        for site in PREDICATE_GUARDS:
            assert image[site:site + 4] == raw[site:site + 4], (name, hex(site))
        if fades:
            # Reserve/write the unchanged complete veneer region once, even
            # when only its fade entries are reachable. No second allocation.
            assert image[stub:stub + code_size] == patched[stub:stub + code_size], name
        else:
            assert image[stub:stub + code_size] == bytes(code_size), name
        if floor:
            assert image[data + 80:data + 88] == bytes(8), name
            assert struct.unpack_from('<Q', image, data + 88)[0] == 200000000, name
        else:
            assert image[data + 80:data + 144] == bytes(64), name
        assert image[data + 96:data + 144] == bytes(48), name
        checked_modes.append({'name': name, 'fade_clamps': fades, 'cutscene_floor': floor})
        if name != 'native60':
            path.unlink()  # own temporary output, retain only the emulated enabled image
    assert patched is not None
    original, port = Guest(raw), Guest(patched)
    # Compare to actual ARM FCVTZS, not a C cast. Include both signs of NaN
    # (quiet/signalling), infinity, saturation, paused +/-0 and subnormals.
    values = [0x00000000, 0x80000000, 0x00000001, 0x80000001,
              0x007fffff, 0x807fffff, 0x00800000, 0x80800000,
              0x3dcccccd, 0xbdcccccd, 0x3f7fffff, 0xbf7fffff,
              0x3f800000, 0xbf800000, 0x40066666, 0xc0066666,
              0x4effffff, 0x4f000000, 0xcf000000, 0xcf000001,
              0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000,
              0x7f800001, 0xff800001]
    fade_count = 0
    for site in FADES:
        rd = struct.unpack_from('<I', raw, site)[0] & 31
        for bits in values:
            before = original.execute(site, bits)
            after = port.execute(site, bits)
            value, = struct.unpack('<f', struct.pack('<I', bits))
            expected = 1 if before[X[rd]] == 0 and not math.isnan(value) and value > 0 else before[X[rd]]
            assert after[X[rd]] == expected, (hex(site), hex(bits), after[X[rd]], expected)
            before[X[rd]] = expected
            assert before == after, (hex(site), hex(bits), 'non-destination register/FP/NZCV/SP changed')
            fade_count += 1
    marker_count = interval_count = 0
    for index, marker in enumerate(MARKERS):
        port.mu.mem_write(data + 80, bytes(8))
        port.clock += 1000000000
        assert original.execute(marker) == port.execute(marker), (hex(marker), 'entry instruction/register ABI')
        marker_stamp, = struct.unpack('<Q', port.mu.mem_read(data + 96 + index * 8, 8))
        assert marker_stamp == port.clock
        stamp, = struct.unpack('<Q', port.mu.mem_read(data + 80, 8))
        marker_count += 1
        if index < 3:
            assert stamp == 0, (hex(marker), 'generic entry admitted a floor')
            assert original.execute(0x2db238) == port.execute(0x2db238), 'ordinary openfield interval=1'
            interval_count += 1
            continue
        assert stamp == port.clock
        for age in (-1, 0, 199999999, 200000000, 200000001):
            port.clock = stamp + age
            for candidate in (0, 1, 2, 3, 0xffffffff):
                before = original.execute(0x2db238, candidate=candidate)
                after = port.execute(0x2db238, candidate=candidate)
                applied = max(candidate, 2) if 0 <= age < 200000000 else candidate
                before[X[21]] = applied
                assert before == after, (hex(marker), age, candidate, 'interval/NZCV/SP/register ABI')
                assert struct.unpack('<II', port.mu.mem_read(data + 136, 8)) == (candidate, applied)
                interval_count += 1
        # Generic entries cannot renew an expired admitted camera stamp.
        for generic in MARKERS[:3]:
            port.execute(generic)
        assert original.execute(0x2db238) == port.execute(0x2db238), 'generic updates extended expired floor'
        interval_count += 1
    release_cases = []
    for settings, reason in (({'camera_active': False}, 2), ({'camera_complete': True}, 3)):
        port.clock += 1000000000
        port.execute(MARKERS[3])
        assert port.execute(0x2db238)[X[21]] == 2
        assert original.execute(MARKERS[3], **settings) == port.execute(MARKERS[3], **settings)
        assert struct.unpack('<Q', port.mu.mem_read(data + 80, 8))[0] == 0
        assert struct.unpack('<Q', port.mu.mem_read(data + 128, 8))[0] == reason
        assert original.execute(0x2db238) == port.execute(0x2db238), 'inactive camera entry did not release'
        release_cases.append(settings)
    port.clock += 1000000000
    port.execute(MARKERS[3])
    assert port.execute(0x2db238)[X[21]] == 2
    assert original.execute(0xa5a670) == port.execute(0xa5a670), 'completion store register ABI'
    assert struct.unpack('<I', port.mu.mem_read(port.arena + 0x218, 4))[0] == 0x80000000
    assert struct.unpack('<Q', port.mu.mem_read(data + 80, 8))[0] == 0
    assert struct.unpack('<Q', port.mu.mem_read(data + 128, 8))[0] == 3
    assert original.execute(0x2db238) == port.execute(0x2db238), 'same-call completion did not release'
    release_cases.append({'completion_store': 'same-call'})
    # Every original-site mutation must stop boot; also check occupied veneer
    # and data storage are refused, instead of partially overwriting another patch.
    bad = out / 'invalid-main.bin'
    rejects = []
    try:
        controls = [(site, 'original instruction mismatch') for site in SITES + PREDICATE_GUARDS]
        controls += [(stub + i, 'veneer padding in use') for i in (0, code_size // 2, code_size - 1)]
        guard_modes = [('native60', {'TOTK_FPS60': '1'}),
                       ('framegen-fades', {'TOTK_FPS60': '1', 'TOTK_FRAMEGEN': 'hybrid'}),
                       ('floor-opt-out-fades', {'TOTK_FPS60': '1', 'TOTK_FPS60_HARDENING': '0'})]
        for name, settings in guard_modes:
            for offset, expected in controls:
                image = bytearray(raw)
                assert offset < len(image), 'veneer/source guard is outside the supplied image'
                image[offset] ^= 1
                bad.write_bytes(image)
                rejected = subprocess.run([str(fixture), str(bad), str(out / 'rejected.bin'), hex(args.text_end)],
                    env=env | settings, capture_output=True, text=True)
                assert rejected.returncode and expected in rejected.stderr, (name, hex(offset), rejected.stderr)
                rejects.append({'mode': name, 'offset': hex(offset)})
            for slot in (80, 95, 96, 127, 128, 135, 136, 143):
                rejected = subprocess.run([str(fixture), str(args.image), str(out / 'rejected.bin'),
                    hex(args.text_end), str(slot)], env=env | settings,
                    capture_output=True, text=True)
                assert rejected.returncode and 'data padding in use' in rejected.stderr, (name, slot, rejected.stderr)
                rejects.append({'mode': name, 'offset': 'data+' + str(slot)})
    finally:
        bad.unlink(missing_ok=True)
    report = {'status': 'pass', 'source_sha256': hashlib.sha256(raw).hexdigest(),
              'original_nso_sha256': nso_digest,
              'verified_sha256': {'sites': SITES_SHA256, 'predicates': PREDICATES_SHA256},
              'fade_arm_cases': fade_count, 'marker_entries': marker_count,
              'interval_arm_cases': interval_count, 'mode_identity_and_hle_pacing_cases': checked_modes,
              'camera_release_cases': release_cases, 'ordinary_entry_floor_admission': False,
              'camera_admission': 'nonempty ExecuteCameraMoveCameraAnimationInstructions, status bit31 clear',
              'coverage': 'known scripted-camera action subset, not universal cinematics',
              'diagnostic_coalescing_refreshes': 200,
              'guard_rejections': rejects, 'counter_source': 'deterministic injected CNTVCT reads only',
              'native60_60hz_real_present_floor': 30, 'native60_120hz_display_ticks': 4,
              'expiration_ns': 200000000, 'stub_offset': hex(stub), 'veneer_bytes': code_size}
    (out / 'fps60-hardening-validation.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
