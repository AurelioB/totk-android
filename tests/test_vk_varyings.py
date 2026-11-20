"""Link-time defaults for fragment inputs a vertex shader never declares (issue #4), without a device or game data.

Run with: python3 -m unittest discover -s tests -p test_vk_varyings.py -v
Needs a host C compiler, glslc (Android NDK shader-tools or shaderc) and spirv-val/spirv-dis (SPIRV-Tools).
GLSLC / SPIRV_VAL / SPIRV_DIS override the tool paths.
"""
from pathlib import Path
import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GFX = ROOT / 'games/totk/android/gfx'
CC = shlex.split(os.environ.get('CC', 'cc'))
GLSLC = os.environ.get('GLSLC') or shutil.which('glslc')
SPIRV_VAL = os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')
SPIRV_DIS = os.environ.get('SPIRV_DIS') or shutil.which('spirv-dis')

# Vertex shader in the translator's shape: vec4 outputs, only location 0 declared.
VS = """#version 450
layout(location = 0) in vec4 a0;
layout(location = 0) out vec4 v0;
void main() { v0 = a0; gl_Position = a0; }
"""
# The wind ribbon pattern: alpha multiplied by an input (location 6) the vertex shader never writes,
# plus a flat signed and unsigned integer input it does not write either.
FS = """#version 450
layout(location = 0) in vec4 v0;
layout(location = 6) in vec4 v6;
layout(location = 3) flat in ivec4 v3;
layout(location = 9) flat in uvec4 v9;
layout(location = 0) out vec4 c0;
void main() { c0 = vec4(v0.rgb, v0.a * v6.w) + vec4(float(v3.x + int(v9.y))); }
"""
# Everything the fragment shader reads is written: nothing may change.
VS_FULL = """#version 450
layout(location = 0) in vec4 a0;
layout(location = 0) out vec4 v0;
layout(location = 6) out vec4 v6;
layout(location = 3) flat out ivec4 v3;
layout(location = 9) flat out uvec4 v9;
void main() { v0 = a0; v6 = a0; v3 = ivec4(1); v9 = uvec4(2); gl_Position = a0; }
"""
# Early return and a function-local variable: the default must still be stored first in the entry block.
VS_LOCALS = """#version 450
layout(location = 0) in vec4 a0;
layout(location = 0) out vec4 v0;
void main() { vec4 t = a0 * 2.0; gl_Position = t; if (a0.x > 0.0) { v0 = t; return; } v0 = a0; }
"""
# Indexed fragment inputs (an array) are left to the original behaviour.
FS_ARRAY = """#version 450
layout(location = 0) in vec4 v[8];
layout(location = 0) out vec4 c0;
void main() { c0 = v[0] * v[5].w; }
"""


@unittest.skipUnless(CC and shutil.which(CC[0]) and GLSLC and SPIRV_VAL and SPIRV_DIS,
                     'needs a C compiler, glslc and SPIRV-Tools')
class VaryingDefaultTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        cls.probe = cls.work / 'vkvaryingstest'
        subprocess.run([*CC, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        str(GFX / 'test_vk_varyings.c'), '-o', str(cls.probe)], check=True)

    def compile(self, name, stage, source):
        src = self.work / f'{name}.{stage}'
        src.write_text(source)
        out = self.work / f'{name}.{stage}.spv'
        subprocess.run([GLSLC, '--target-env=vulkan1.1', '-O0', str(src), '-o', str(out)], check=True)
        return out

    def run_probe(self, vs, fs, name):
        out = self.work / f'{name}.patched.spv'
        result = subprocess.run([str(self.probe), str(vs), str(fs), str(out)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        masks = dict(re.findall(r'(vs|fs|array|missing) ([0-9a-f]{8})', result.stdout))
        return {k: int(v, 16) for k, v in masks.items()}, out

    def test_missing_inputs_get_maxwell_default(self):
        masks, out = self.run_probe(self.compile('wind', 'vert', VS), self.compile('wind', 'frag', FS), 'wind')
        self.assertEqual(masks['vs'], 1 << 0)
        self.assertEqual(masks['missing'], 1 << 3 | 1 << 6 | 1 << 9)
        subprocess.run([SPIRV_VAL, '--target-env', 'vulkan1.1', str(out)], check=True)
        dis = subprocess.run([SPIRV_DIS, '--raw-id', str(out)], check=True, capture_output=True, text=True).stdout
        for location in (3, 6, 9):
            self.assertRegex(dis, rf'OpDecorate %\d+ Location {location}\n')
        # w = 1.0f bits for every component type, as the translator's own output initialization
        # (spirv-dis prints the float one as 1; the integer types carry the raw bits)
        self.assertEqual(len(re.findall(r'OpConstant %\d+ 1065353216\n', dis)), 2)
        self.assertEqual(len(re.findall(r'OpConstantComposite %\d+ (%\d+) \1 \1 %\d+\n', dis)), 3)
        entry = re.search(r'OpEntryPoint Vertex %\d+ "main"(.*)\n', dis).group(1)
        self.assertEqual(len(entry.split()), 3 + 3)   # gl_PerVertex, a0, v0, plus the three new outputs

    def test_complete_vertex_shader_unchanged(self):
        masks, out = self.run_probe(self.compile('full', 'vert', VS_FULL), self.compile('wind2', 'frag', FS), 'full')
        self.assertEqual(masks['missing'], 0)
        self.assertFalse(out.exists())

    def test_locals_and_early_return(self):
        masks, out = self.run_probe(self.compile('locals', 'vert', VS_LOCALS), self.compile('wind3', 'frag', FS), 'locals')
        self.assertEqual(masks['missing'], 1 << 3 | 1 << 6 | 1 << 9)
        subprocess.run([SPIRV_VAL, '--target-env', 'vulkan1.1', str(out)], check=True)
        dis = subprocess.run([SPIRV_DIS, '--raw-id', str(out)], check=True, capture_output=True, text=True).stdout
        body = dis[dis.index('OpFunction '):]
        first_block = body[body.index('OpLabel'):].split('\n')[1:]
        ops = [line.split('=')[-1].split()[0] for line in first_block if line.strip()]
        # function-local variables stay first, then the three default stores
        self.assertEqual(ops[0], 'OpVariable')
        stores = [i for i, op in enumerate(ops) if op == 'OpStore']
        non_var = next(i for i, op in enumerate(ops) if op != 'OpVariable')
        self.assertEqual(stores[:3], [non_var, non_var + 1, non_var + 2])

    def test_array_inputs_left_alone(self):
        masks, out = self.run_probe(self.compile('arr', 'vert', VS), self.compile('arr', 'frag', FS_ARRAY), 'arr')
        self.assertEqual(masks['array'], 0xff)
        self.assertEqual(masks['missing'], 0)
        self.assertFalse(out.exists())


if __name__ == '__main__':
    unittest.main()
