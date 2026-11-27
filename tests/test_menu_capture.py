"""Production-C menu patch guards and player-supplied ARM64 contract checks.

No game words in the repository. Semantic checks require TOTK_MAIN_NSO and
Unicorn/lz4; they prove admission/register/flag behavior, not job ordering.
"""
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / 'games/totk/scripts'
MAIN_NSO = Path(os.environ.get('TOTK_MAIN_NSO', ROOT.parent / 'build/TOTK_build/totk/extracted/v143/exefs/main'))
SITE, UPDATE, CONSUME = 0x105bbcc, 0x105ba70, 0x105c36c
GUARD = 'a5e2a3003d86bfbb936dc67e51990369aea8bf463786eea310b99463fc3a901d'
HARDENING_SITES = [
    0x0a7ac4, 0x95dd24, 0x6155e4, 0x615660, 0x45c700, 0x2b23c7c,
    0x42c3bc, 0xa5a188, 0x2db238, 0xa5a670,
    0xa5a18c, 0xa5a660, 0xa5a664, 0xa5a668, 0xa5a66c, 0xa5a674,
]


class MenuCapture(unittest.TestCase):
    def setUp(self):
        sys.path.insert(0, str(SCRIPTS))
        self.addCleanup(sys.path.remove, str(SCRIPTS))
        import fixture_seeds
        self.seeds = fixture_seeds

    def run_fixture(self, executable, args, seeds=None):
        try:
            return self.seeds.run(executable, args, seeds)
        except subprocess.CalledProcessError as error:
            self.fail(f'fixture exited {error.returncode}:\n{error.stdout}\n{error.stderr}')

    def test_production_guards(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'menu-fixture'
            self.seeds.build(SCRIPTS / 'v143_menu_capture_fixture.c', executable, standin=True)
            result = self.run_fixture(executable, [])
            self.assertIn('menu capture guards PASS', result.stdout)

    def test_ios_runtime_and_writable_export_leave_complete_image_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            for phone in (False, True):
                source = directory / 'platform.c'
                source.write_text(('#define TARGET_OS_IPHONE 1\n#define MENU_CAPTURE_NATIVE_HOST 1\n' if phone else '')
                                  + f'#include "{SCRIPTS / "v143_menu_capture_fixture.c"}"\n')
                executable = self.seeds.build(source, directory / 'platform', standin=True)
                with mock.patch.dict(os.environ, {'TOTK_EXPORT_IOS': str(directory / 'export')}):
                    result = self.run_fixture(executable, ['unsupported'])
                self.assertIn('complete image unchanged', result.stdout)
                self.assertIn('inactive: iOS runtime' if phone else 'inactive: iOS export', result.stderr)

    def test_non_v143_startup_does_not_read_menu_switch_or_change_image(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            for version in ('', 'V112', 'V121'):
                source = directory / 'legacy.c'
                source.write_text((f'#define TOTK_GAME_{version} 1\n' if version else '')
                                  + f'#include "{SCRIPTS / "legacy_menu_capture_fixture.c"}"\n')
                executable = self.seeds.build(source, directory / 'legacy', standin=True)
                self.assertIn('legacy menu image unchanged', self.run_fixture(executable, []).stdout)

    @unittest.skipUnless(importlib.util.find_spec('unicorn') and importlib.util.find_spec('lz4') and MAIN_NSO.is_file(),
                         'Unicorn/lz4 and the 1.4.3 main NSO ($TOTK_MAIN_NSO) required')
    def test_player_supplied_arm64_contract(self):
        from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
        from unicorn import arm64_const as reg
        original = self.seeds.read_main(MAIN_NSO)
        self.seeds.check(original, [SITE, SITE + 4], GUARD)
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            seeds = self.seeds.write_seeds(directory / 'seeds.bin', original, [SITE, SITE + 4, *HARDENING_SITES])
            executable = self.seeds.build(SCRIPTS / 'v143_menu_capture_fixture.c', directory / 'fixture')
            images = {}
            explicit_on = b''
            for enabled in (0, 1, None):
                image_path = directory / f'patched-{enabled}.bin'
                self.run_fixture(executable, [image_path, *([] if enabled is None else [str(enabled)])], seeds)
                image = bytearray(image_path.read_bytes())
                if enabled == 1:
                    explicit_on = bytes(image)
                if enabled is None:
                    self.assertEqual(image, explicit_on, 'unset must install exactly the explicit-on image')
                    continue
                # Supply native dispatch, continuation, update and completion;
                # only the production installer decides admission.
                image[SITE + 4:0x105bc10] = original(SITE + 4, 0x105bc10 - SITE - 4)
                image[UPDATE:0x105bba8] = original(UPDATE, 0x105bba8 - UPDATE)
                image[CONSUME:0x105c388] = original(CONSUME, 0x105c388 - CONSUME)
                images[enabled] = image
        registers = [getattr(reg, f'UC_ARM64_REG_X{i}') for i in range(31)]
        vectors = [getattr(reg, f'UC_ARM64_REG_Q{i}') for i in range(32)]
        pane, context, data, stop = 0x10000000, 0x10001000, 0x10002000, 0x10008000

        def machine(request, armed, vis, alpha, flags=0, enabled=1):
            mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
            mu.mem_map(0, len(images[enabled]))
            mu.mem_write(0, bytes(images[enabled]))
            mu.mem_map(pane, 0x10000)
            for i, register in enumerate(registers):
                mu.reg_write(register, 0x123400000000 + i)
            for i, register in enumerate(vectors):
                mu.reg_write(register, 0x123456789abcdef0123456789abcdef0 + i)
            mu.reg_write(reg.UC_ARM64_REG_X0, pane)
            mu.reg_write(reg.UC_ARM64_REG_X1, context)
            mu.reg_write(reg.UC_ARM64_REG_X2, data)
            mu.reg_write(reg.UC_ARM64_REG_X30, stop)
            mu.reg_write(reg.UC_ARM64_REG_SP, 0x1000f000)
            mu.reg_write(reg.UC_ARM64_REG_NZCV, 0xa0000000)
            mu.mem_write(pane + 0x58, bytes([vis, alpha]))
            mu.mem_write(pane + 0xd2, struct.pack('<HBB', flags, request, armed))
            mu.hook_add(UC_HOOK_CODE, lambda cpu, address, size, user: cpu.emu_stop() if address == stop else None)
            return mu

        cases = 0
        for enabled in (0, 1):
            for request in (0, 1, 255):
                for armed in (0, 1, 255):
                    for vis, alpha in ((0, 0), (0, 1), (1, 0), (1, 1), (64, 1), (65, 2)):
                        mu = machine(request, armed, vis, alpha, enabled=enabled)
                        before = [mu.reg_read(r) for r in registers]
                        before_vectors = [mu.reg_read(r) for r in vectors]
                        before_memory = bytes(mu.mem_read(pane, 0x10000))
                        mu.emu_start(SITE, SITE + 4, count=20)
                        self.assertEqual(mu.reg_read(reg.UC_ARM64_REG_PC), SITE + 4)
                        expected = armed | request if enabled else armed
                        self.assertEqual(mu.reg_read(reg.UC_ARM64_REG_X8), expected)
                        self.assertEqual(mu.reg_read(reg.UC_ARM64_REG_X9), request if enabled else before[9])
                        self.assertEqual([mu.reg_read(r) for i, r in enumerate(registers) if i not in (8, 9)],
                                         [v for i, v in enumerate(before) if i not in (8, 9)])
                        self.assertEqual([mu.reg_read(r) for r in vectors], before_vectors)
                        self.assertEqual(mu.reg_read(reg.UC_ARM64_REG_NZCV), 0xa0000000)
                        self.assertEqual(mu.reg_read(reg.UC_ARM64_REG_SP), 0x1000f000)
                        self.assertEqual(bytes(mu.mem_read(pane, 0x10000)), before_memory)
                        # Execute the real consumer dispatch; no synthetic predicate.
                        mu.emu_start(SITE + 4, stop, count=1)
                        self.assertEqual(mu.reg_read(reg.UC_ARM64_REG_PC), 0x105bbf8 if expected else 0x105bbd4)
                        if expected:
                            for resource in (0, pane + 0x3000):
                                mu.mem_write(context + 0x1a0, struct.pack('<Q', resource))
                                mu.mem_write(pane + 0x3000, struct.pack('<Q', pane + 0x4000))
                                mu.emu_start(0x105bbf8, stop, count=3)
                                self.assertEqual(mu.reg_read(reg.UC_ARM64_REG_PC), 0x105bc04 if resource else 0x105bbd4)
                                if resource:
                                    mu.emu_start(0x105bc04, 0x105bc10, count=3)
                                    self.assertEqual(mu.reg_read(reg.UC_ARM64_REG_X9), 0x3968000)
                        cases += 1
        self.assertEqual(cases, 108)

        # Authenticate the native eligibility gate: request OR continuous bit 9,
        # then pane-visible bit 0 and nonzero effective alpha. Stop before calls
        # which prepare state; these tests do not invent those calls' behavior.
        for request in (0, 1):
            for continuous in (0, 1):
                for vis, alpha in ((0, 1), (1, 0), (1, 1), (64, 1), (65, 2)):
                    mu = machine(request, 0, vis, alpha, continuous << 9)
                    mu.emu_start(UPDATE, 0x105badc, count=100)
                    eligible = bool((request or continuous) and vis & 1 and alpha)
                    self.assertEqual(mu.mem_read(pane + 0xd5, 1), bytes([int(eligible)]))
                    self.assertEqual(mu.reg_read(reg.UC_ARM64_REG_PC), 0x105badc if eligible else stop)

        # Completion does NOT unconditionally clear request. Bit 6 deliberately
        # republishes it; hence there is no static once-per-opening cost bound.
        for flags in (0, 1, 64, 65, 512, 576, 65535):
            mu = machine(1, 1, 1, 1, flags)
            mu.reg_write(reg.UC_ARM64_REG_X19, pane)
            mu.emu_start(CONSUME, 0x105bbd4, count=8)
            self.assertEqual(mu.reg_read(reg.UC_ARM64_REG_PC), 0x105bbd4)
            self.assertEqual(mu.mem_read(pane + 0xd4, 2), bytes([(flags >> 6) & 1, 0]))
            self.assertEqual(struct.unpack('<H', mu.mem_read(pane + 0xd2, 2))[0], flags | 1)
