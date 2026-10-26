"""Small real-tool fixtures for the opt-in seed optimizer (no game/device needed)."""
import importlib.util
from pathlib import Path
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace

ANDROID = Path(__file__).resolve().parents[1] / 'games/totk/android'
sys.path.insert(0, str(ANDROID))
spec = importlib.util.spec_from_file_location('seed_opt', ANDROID / 'optimize_shader_seed.py')
opt = importlib.util.module_from_spec(spec)
GLSLANG = shutil.which('glslangValidator')
GLSLC = os.environ.get('GLSLC') or shutil.which('glslc')
spec.loader.exec_module(opt)

@unittest.skipUnless(all(shutil.which(x) for x in ('spirv-opt', 'spirv-val')) and (GLSLANG or GLSLC), 'needs SPIRV-Tools and glslang/glslc')
class SeedOptimizationTests(unittest.TestCase):
    def fixture(self, root):
        source = root / 'input'
        source.mkdir()
        glsl = root / 'shader.frag'
        glsl.write_text('''#version 450
layout(early_fragment_tests) in;
layout(constant_id=123) const float nudge = 0.0;
layout(set=0,binding=0) uniform sampler2D fp_t_tcb_5A;
layout(set=0,binding=1) writeonly uniform image2D fp_i_tcb_48;
layout(location=0) out vec4 color;
void main() {
  vec2 uv = gl_FragCoord.xy / 64.0 + vec2(0.0001);
  color = textureGather(fp_t_tcb_5A, uv);
  gl_FragDepth = gl_FragCoord.z + nudge;
  imageStore(fp_i_tcb_48, ivec2(gl_FragCoord.xy), color);
}
''')
        spv = source / 'variant.spv'
        command = ([GLSLANG, '-V', '--target-env', 'vulkan1.2'] if GLSLANG else [GLSLC, '--target-env=vulkan1.2'])
        subprocess.run([*command, str(glsl), '-o', str(spv)], check=True, capture_output=True)
        data = spv.read_bytes()
        r = opt.vk_reflect.reflect(data, {}, 1)
        spv.with_suffix('.vk').write_text(opt.vk_reflect.vk_text(r, 1, 'abcd'))
        (source / 'key.vv').write_text('variant\tfout:0=float\n')
        return source, data

    def test_legalization_preserves_interface_and_metadata(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source, data = self.fixture(root)
            # Reproduce the seed emitter's precise missing-declaration defect.
            words = list(struct.unpack(f'<{len(data)//4}I', data))
            i = 5
            while i < len(words):
                n = words[i] >> 16
                if words[i] & 0xffff == 17 and words[i + 1] == 56:
                    del words[i:i+n]
                    break
                i += n
            else:
                self.fail('fixture must declare StorageImageWriteWithoutFormat')
            broken = struct.pack(f'<{len(words)}I', *words)
            (source / 'variant.spv').write_bytes(broken)
            output = root / 'optimized'
            args = SimpleNamespace(source=source, output=output, jobs=1, spirv_opt='spirv-opt', spirv_val='spirv-val')
            opt.run(args)
            self.assertEqual((source / 'variant.spv').read_bytes(), broken)
            for name in ('variant.vk', 'key.vv'):
                self.assertEqual((source / name).read_bytes(), (output / name).read_bytes())
            self.assertEqual(opt.signature(data, 1), opt.signature((output / 'variant.spv').read_bytes(), 1))
            import json
            report = json.loads((output / 'optimization.json').read_text())
            self.assertEqual(report['validated'], 1)
            self.assertEqual(report['legalized_storage_image_write_capability'], 1)
            self.assertEqual(len((output / 'seed-id.txt').read_text().strip()), 64)
            with self.assertRaisesRegex(ValueError, 'already exists'):
                opt.run(args)
            args.output = source
            with self.assertRaisesRegex(ValueError, 'must not overlap'):
                opt.run(args)


if __name__ == '__main__':
    unittest.main()
