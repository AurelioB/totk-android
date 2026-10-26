"""Run the production Vulkan variant matcher without a Vulkan device."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT / 'games/totk'


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('cc'), 'needs Linux cc')
class ShaderDependencyTests(unittest.TestCase):
    def test_texture_depth_class_wildcards(self):
        with tempfile.TemporaryDirectory() as temp:
            probe = Path(temp) / 'shader-deps'
            subprocess.run([
                'cc', '-std=gnu11', '-D_GNU_SOURCE', '-DGPU_VK', '-O1',
                '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                '-I' + str(TOTK / 'runtime'),
                '-I' + str(TOTK / 'third_party/d2r_gfx/gfx/nvn'),
                '-I' + str(TOTK / 'third_party/d2r_gfx/gfx/nvn/include'),
                '-I' + str(TOTK / 'third_party/d2r_gfx/host'),
                '-I' + str(TOTK / 'android/compat/include'),
                '-include', str(TOTK / 'android/compat/include/darwin_compat.h'),
                str(TOTK / 'android/gfx/test_vk_shader_deps.c'), '-o', str(probe),
            ], check=True, capture_output=True, text=True)
            run = subprocess.run([str(probe)], check=True, capture_output=True, text=True)
            self.assertIn('wildcard matching passed', run.stdout)


if __name__ == '__main__':
    unittest.main()
