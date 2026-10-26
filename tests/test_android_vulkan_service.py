"""Exercise the actual NativeAOT translator, not a protocol stub (no game data).
Set TOTK_TEST_XLATE to a freshly published linux-x64 translator to enable.
"""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'games/totk/android'))
import vk_reflect

TRANSLATOR = os.environ.get('TOTK_TEST_XLATE')


@unittest.skipUnless(TRANSLATOR, 'set TOTK_TEST_XLATE to the NativeAOT host translator')
class VulkanShaderServiceTests(unittest.TestCase):
    def test_direct_service_matches_existing_pc_reflection(self):
        code = struct.pack('<4Q', 0, 0xe30000000007000f, 0, 0)
        variant = 'a' * 64 + '-' + 'b' * 12
        request = dict(variant=variant, stage='cs', code_hex=code.hex(), c1='', local=[1, 1, 1],
                       attr={}, fout={}, img={}, tex={}, depth_minus_w=False)
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            out = folder / 'vulkan'
            service = subprocess.Popen([TRANSLATOR, '--service', '--vulkan', '--out', str(out), '--chain', 'test'],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                def send(value):
                    service.stdin.write(json.dumps(value) + '\n')
                    service.stdin.flush()
                    return json.loads(service.stdout.readline())
                self.assertTrue(send(request)['ok'])
                spv = (out / f'{variant}.spv').read_bytes()
                self.assertEqual(struct.unpack_from('<I', spv)[0], 0x07230203)
                self.assertIn('local 1 1 1\n', (out / f'{variant}.vk').read_text())
                self.assertEqual((out / ('a' * 64 + '.vv')).read_text(), variant + '\t\n')
                self.assertTrue(send(request)['ok'])
                self.assertEqual((out / ('a' * 64 + '.vv')).read_text(), variant + '\t\n', 'duplicate request must not duplicate index rows')
                bad = dict(request, stage='bad')
                self.assertFalse(send(bad)['ok'])
                self.assertTrue(send(request)['ok'], 'rejected request must not kill service')
                # .NET 10's JsonDocument duplicate-name hash set (>16 properties) used to root OpenSSL RNG.
                wide = dict(request, tex={str(i): '2d' for i in range(32)},
                            ignored_note='escaped \\t \u5b57', ignored_unsigned=2**64 - 1,
                            ignored_negative=-(2**63), ignored_fraction=0.001)
                self.assertTrue(send(wide)['ok'])
                self.assertEqual(spv, (out / f'{variant}.spv').read_bytes())
                for invalid in ('{"stage":"cs","stage":"vs"}', '{} {}', '{"unclosed":[1'):
                    service.stdin.write(invalid + '\n'); service.stdin.flush()
                    self.assertFalse(json.loads(service.stdout.readline())['ok'])
                self.assertTrue(send(request)['ok'], 'invalid JSON must not poison the next service request')
                code_path = folder / 'tiny.code'; code_path.write_bytes(code)
                legacy = folder / 'legacy'; legacy.mkdir()
                job = dict(request, key=variant, code=str(code_path), debug=True, reduced=True, gather_bias=True)
                job.pop('code_hex')
                result = subprocess.run([TRANSLATOR, '--serve'], input=json.dumps(dict(job=job, out=str(legacy))) + '\n',
                                        text=True, capture_output=True, check=True)
                self.assertTrue(result.stdout.startswith('ok '), result.stdout + result.stderr)
                self.assertEqual(spv, (legacy / f'{variant}.spv').read_bytes())
                resources = json.loads((legacy / f'{variant}.res.json').read_text())
                reflection = vk_reflect.reflect(spv, resources, 5)
                reflection['local'] = (1, 1, 1)
                self.assertEqual((out / f'{variant}.vk').read_text(), vk_reflect.vk_text(reflection, 5, ''))
                self.assertEqual(sorted(p.suffix for p in out.iterdir()), ['.spv', '.vk', '.vv'])
            finally:
                service.stdin.close()
                service.wait(timeout=10)
                error = service.stderr.read()
                service.stdout.close(); service.stderr.close()
            self.assertEqual(service.returncode, 0, error)


if __name__ == '__main__':
    unittest.main()
