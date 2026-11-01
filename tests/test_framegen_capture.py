"""Production adaptive capture routing plus optional actual Vulkan walker fixture.

The source contracts cover app-only callsites unavailable to the host presenter;
the --adaptive-capture executable exercises real walker/native-UI/SMAA GPU work
with genuine host AFME refusal, never a replacement generated-output backend.
"""
import os
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
DRAW = ROOT / 'games/totk/android/gfx/vk_draw.c'


def function_body(source, name):
    match = re.search(r'\b' + re.escape(name) + r'\s*\([^;{}]*\)\s*\{', source)
    if not match:
        raise AssertionError(f'missing production function {name}')
    start = match.end() - 1
    depth = 0
    tokens = re.finditer(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|[{}]',
                         source[start:], re.S)
    for token in tokens:
        value = token.group()
        if value == '{':
            depth += 1
        elif value == '}':
            depth -= 1
            if not depth:
                return source[start + 1:start + token.start()]
    raise AssertionError(f'unclosed production function {name}')


class FramegenCapture(unittest.TestCase):
    def test_actual_present_uses_shared_capture_and_end_selectors(self):
        source = DRAW.read_text()
        selector = function_body(source, 'fg_extract_requested_config')
        self.assertRegex(selector, r'!strcmp\(e,\s*"adaptive"\)')
        refresh = function_body(source, 'fg_extract_refresh')
        self.assertIn('fg_extract_requested_config()', refresh)
        self.assertIn('vk_fg_capturing()', refresh)
        self.assertIn('fg_extract_requested_config()', function_body(source, 'vk_fg_present_begin'))
        self.assertIn('vk_fg_extract_present(t)', function_body(source, 'vk_fg_present_begin'))
        end = function_body(source, 'vk_fg_present_end')
        self.assertIn('fg_extract_requested_config()', end)
        self.assertIn('vk_fg_extract_end()', end)
        self.assertIn('vk_fg_frame_end(fence)', end)
        present = function_body(source, 'mtl_present')
        self.assertIn('vk_fg_present_begin(t)', present)
        self.assertIn('vk_fg_present_end(vk_submit_now())', present)
        self.assertIn('vk_fg_present_begin(t)', function_body(source, 'vk_fg_test_present_begin'))
        self.assertIn('vk_fg_present_end(fence)', function_body(source, 'vk_fg_test_present_end'))

    def test_generated_smaa_refreshes_primary_after_external_split(self):
        body = function_body(DRAW.read_text(), 'present_generated')
        producer = body.index('vk_fg_generate_phase(')
        post = body.index('vk_post_run(', producer)
        self.assertRegex(body[producer:post], r'cb\s*=\s*vk_cb\s*\(\s*\)\s*;')
        self.assertRegex(body[post:], r'vk_post_run\(cb,')

    def test_dropped_real_uses_same_temporal_cancel_as_host_fixture(self):
        source = DRAW.read_text()
        cancel = function_body(source, 'fg_adaptive_present_cancel')
        self.assertIn('runtime_fps_adaptive_reset()', cancel)
        self.assertIn('vk_fg_skip()', cancel)
        self.assertIn('fg_adaptive_present_cancel()', function_body(source, 'vk_fg_test_present_cancel'))
        self.assertIn('fg_adaptive_present_cancel()', function_body(source, 'present_window'))

    def test_actual_adaptive_walker_native_ui_and_smaa(self):
        executable = os.environ.get('TOTK_VKFRAMEGENTEST')
        seed = os.environ.get('TOTK_SHADER_SEED_DIR')
        if not executable or not seed:
            self.skipTest('Set TOTK_VKFRAMEGENTEST and TOTK_SHADER_SEED_DIR for actual Vulkan capture acceptance')
        executable = Path(executable).resolve()
        self.assertTrue(executable.is_file() and os.access(executable, os.X_OK), str(executable))
        self.assertTrue((Path(seed) / '.done').is_file(), 'complete owned vkdraw shader seed is required')
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(('TOTK_', 'DM_'))}
        env['TOTK_SHADER_SEED_DIR'] = str(Path(seed).resolve())
        result = subprocess.run([str(executable), '--adaptive-capture'], env=env,
                                capture_output=True, text=True, timeout=180)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('adaptive capture: actual640-wide walker/nativeUI/history/frame-end', result.stdout)
        self.assertIn('no AFME output claim', result.stdout)


if __name__ == '__main__':
    unittest.main()
