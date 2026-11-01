"""Compare fresh production startup gates to the frozen 9fa08bd7 legacy policy.

No game, Vulkan initialization, private DLL, device or git history is required.
Set TOTK_VKFRAMEGENTEST to an already-built vkframegentest to reuse Main's build;
otherwise this suite builds the existing host target in a temporary directory.
"""
import itertools
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def legacy_gate(env):
    """Literal string semantics frozen from 9fa08bd7; intentionally not tightened.

    Sources: gfx/vk_framegen.c vk_fg_enabled/interpolating/locked/capturing/
    lead_refreshes and runtime/fps_hybrid_runtime.c configure. The host probe
    uses TOTK_VK_TESTS, matching that original hybrid platform/version exception.
    Hybrid starts native-only before any acquire/controller observations.
    """
    def nonzero(key):
        return env.get(key) not in (None, '', '0')

    def differs(key, expected):
        return key in env and env[key] != expected

    mode = env.get('TOTK_FRAMEGEN')
    hybrid = mode == 'hybrid'
    hybrid_selected = hybrid and env.get('TOTK_FPS60') == '1'
    hybrid_selected = hybrid_selected and not (
        nonzero('TOTK_FPS_AUTO') or nonzero('TOTK_UNCAPPED') or
        nonzero('DM_DETCLOCK') or differs('TOTK_REALTIME', '1') or
        differs('TOTK_PRESENT_SLOTS', '1') or differs('DM_PRESENT_HZ', '60') or
        differs('DM_PRESENT_INTERVAL', 'game') or nonzero('TOTK_ASYNC_REPLAY') or
        nonzero('TOTK_ACQUIRE_SLACK') or nonzero('TOTK_ACQUIRE_CLOCK') or
        differs('TOTK_PACE', '0') or nonzero('TOTK_MFX'))
    locked = mode == 'locked'
    config = (3 if hybrid_selected else 0) if hybrid else (
        4 if locked else 2 if mode == 'interp' else int(mode == '1'))
    if config and (
            (not hybrid and differs('TOTK_FPS60', '0')) or
            differs('TOTK_UNCAPPED', '0') or env.get('TOTK_FPS_AUTO') == '1' or
            differs('TOTK_PRESENT_SLOTS', '1') or
            (locked and env.get('TOTK_REALTIME') != '1') or
            (locked and env.get('TOTK_FRAMEGEN_BUDGET_MS') not in (None, '')) or
            (locked and 'TOTK_FRAMEGEN_LEAD_REFRESHES' in env and
             env['TOTK_FRAMEGEN_LEAD_REFRESHES'] not in ('1', '2'))):
        config = 0
    return {
        'enabled': int(config != 0),
        'interpolating': int(config >= 2),
        'locked': int(config == 4),
        'capturing': int(config != 0 and config != 3),
        'lead_refreshes': 2 if env.get('TOTK_FRAMEGEN_LEAD_REFRESHES') == '2' else 1,
        'hybrid_selected': int(hybrid_selected),
        'adaptive': 0,
    }


def gate_cases():
    keys = ('TOTK_FPS60', 'TOTK_UNCAPPED', 'TOTK_FPS_AUTO',
            'TOTK_PRESENT_SLOTS', 'TOTK_REALTIME', 'TOTK_FRAMEGEN_BUDGET_MS',
            'TOTK_FRAMEGEN_LEAD_REFRESHES')
    invalid = ('invalid', '1', '1', '0', '0', '10', '3')
    # Cartesian valid/refusing inputs exercise masked and simultaneous failures,
    # not just one happy path or one changed flag per process. Camera's actual
    # legacy selector is "1"; the literal "camera" is deliberately still off.
    for mode in ('locked', 'hybrid', 'interp', '1'):
        base = {'TOTK_FRAMEGEN': mode, 'TOTK_FPS60': '1' if mode == 'hybrid' else '0',
                'TOTK_UNCAPPED': '0', 'TOTK_FPS_AUTO': '0',
                'TOTK_PRESENT_SLOTS': '1', 'TOTK_REALTIME': '1'}
        for choices in itertools.product((False, True), repeat=len(keys)):
            env = base.copy()
            for key, bad, selected in zip(keys, invalid, choices):
                if selected:
                    env[key] = bad
            yield f'{mode}/cartesian/{choices}', env
        for key in keys:
            for value in (None, '', '0', '1', '2', '3', '-1', '01', 'true', ' 1', '1 '):
                env = base.copy()
                if value is None:
                    env.pop(key, None)
                else:
                    env[key] = value
                yield f'{mode}/{key}={value!r}', env
        for value in (None, '', 'camera', 'qcom', 'lsfg', 'QCOM', 'invalid'):
            env = base.copy()
            if value is not None:
                env['TOTK_FRAMEGEN_ENGINE'] = value
            yield f'{mode}/engine={value!r}', env
        # Unset optional controls preserve their original defaults. Locked alone
        # still requires realtime=1, while hybrid requires FPS60=1 explicitly.
        yield f'{mode}/all-unset', {'TOTK_FRAMEGEN': mode}
        if mode == 'hybrid':
            for key in ('DM_DETCLOCK', 'DM_PRESENT_HZ', 'DM_PRESENT_INTERVAL',
                        'TOTK_ASYNC_REPLAY', 'TOTK_ACQUIRE_SLACK',
                        'TOTK_ACQUIRE_CLOCK', 'TOTK_PACE', 'TOTK_MFX'):
                for value in (None, '', '0', '1', '2', '60', 'game', 'invalid'):
                    env = base.copy()
                    if value is not None:
                        env[key] = value
                    yield f'hybrid/aux/{key}={value!r}', env
    for mode in (None, '', '0', 'camera', 'Camera', 'locked1', 'LOCKED',
                 'hybrid1', 'Hybrid', 'interp1', 'Interp', 'true', '01',
                 'adaptive1', 'Adaptive', ' adaptive', 'adaptive '):
        env = {} if mode is None else {'TOTK_FRAMEGEN': mode}
        yield f'exact-selector/{mode!r}', env


class FramegenLegacyGate(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        supplied = os.environ.get('TOTK_VKFRAMEGENTEST')
        if supplied:
            cls.executable = Path(supplied).resolve()
            if not cls.executable.is_file() or not os.access(cls.executable, os.X_OK):
                raise AssertionError(f'TOTK_VKFRAMEGENTEST is not executable: {cls.executable}')
            return
        compiler = shutil.which('clang') or shutil.which('cc')
        make = shutil.which('make')
        if not compiler or not make:
            raise unittest.SkipTest('A C compiler and make are required')
        scratch = tempfile.TemporaryDirectory(prefix='totk-framegen-gates-')
        cls.addClassCleanup(scratch.cleanup)
        out = Path(scratch.name)
        build = subprocess.run(
            [make, '-C', str(ROOT / 'games/totk/android'), '-s', 'TARGET=linux',
             f'CC={compiler}', f'OUT={out}', 'vkframegentest'],
            capture_output=True, text=True, timeout=600)
        if build.returncode:
            raise AssertionError(build.stdout + build.stderr)
        cls.executable = out / 'vkframegentest'

    def test_frozen_legacy_gate_matrix(self):
        # Do not inherit unrelated startup overrides from the caller's game shell.
        # Keep loader/compiler host environment such as PATH/LD_LIBRARY_PATH.
        clean = {key: value for key, value in os.environ.items()
                 if not key.startswith(('TOTK_', 'DM_'))}
        seen = set()
        for label, controls in gate_cases():
            identity = tuple(sorted(controls.items()))
            if identity in seen:
                continue
            seen.add(identity)
            with self.subTest(case=label, controls=controls):
                result = subprocess.run(
                    [str(self.executable), '--fg-gate'], env=clean | controls,
                    capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                records = [line.removeprefix('FG_GATE ') for line in result.stdout.splitlines()
                           if line.startswith('FG_GATE ')]
                self.assertEqual(len(records), 1, result.stdout + result.stderr)
                self.assertEqual(json.loads(records[0]), legacy_gate(controls),
                                 result.stdout + result.stderr)
        self.assertGreater(len(seen), 700, 'matrix must retain its multi-flag and malformed-input coverage')

    def test_adaptive_scheduler_budget_gate_preserves_flag_off_policy(self):
        # Frozen signed 6cb44c33 / pre-F1 adaptive startup policy: a budget
        # override did not disable adaptive. Scheduler1 alone adds this gate.
        clean = {key: value for key, value in os.environ.items()
                 if not key.startswith(('TOTK_', 'DM_'))}
        controls = {'TOTK_FRAMEGEN': 'adaptive', 'TOTK_FRAMEGEN_ENGINE': 'qcom',
                    'TOTK_FPS60': '1', 'TOTK_REALTIME': '1',
                    'TOTK_PRESENT_SLOTS': '1', 'TOTK_NATIVE_UI': '1'}
        pre_scheduler = {'enabled': 1, 'interpolating': 1, 'locked': 0,
                         'capturing': 1, 'lead_refreshes': 1,
                         'hybrid_selected': 0, 'adaptive': 1}
        disabled = pre_scheduler | {'enabled': 0, 'interpolating': 0,
                                    'capturing': 0, 'adaptive': 0}
        for selector in (None, '0', '1'):
            for budget in (None, '', '23', '0', 'invalid'):
                env = controls.copy()
                if selector is not None:
                    env['TOTK_FG_ADAPTIVE_SLOTS'] = selector
                if budget is not None:
                    env['TOTK_FRAMEGEN_BUDGET_MS'] = budget
                with self.subTest(selector=selector, budget=budget):
                    # Each subprocess initializes fresh cached startup state.
                    result = subprocess.run([str(self.executable), '--fg-gate'],
                                            env=clean | env, capture_output=True,
                                            text=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    records = [line.removeprefix('FG_GATE ')
                               for line in result.stdout.splitlines()
                               if line.startswith('FG_GATE ')]
                    self.assertEqual(len(records), 1, result.stdout + result.stderr)
                    expected = disabled if selector == '1' and budget else pre_scheduler
                    self.assertEqual(json.loads(records[0]), expected,
                                     result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
