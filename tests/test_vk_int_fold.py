"""Exact integer address-bit propagation, without Vulkan or game data."""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GFX = ROOT / 'games/totk/android/gfx'
CC = shlex.split(os.environ.get('CC', 'cc'))


@unittest.skipUnless(CC and shutil.which(CC[0]), 'needs a host C compiler')
class IntegerFoldTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = ROOT / '.scratch'
        scratch.mkdir(exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        cls.probe = cls.work / 'vkintfoldtest'
        subprocess.run([*CC, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(GFX / 'test_vk_int_fold.c'), '-o', str(cls.probe)], check=True)

    def test_bit_algebra_and_conservative_boundaries(self):
        result = subprocess.run([str(self.probe)], check=True, capture_output=True, text=True)
        self.assertIn('boundaries, escape, volatile, malformed PASS', result.stdout)

    @unittest.skipUnless(shutil.which('spirv-as') and shutil.which('spirv-val'), 'needs SPIR-V tools')
    def test_valid_module_and_wraparound_address(self):
        source = self.work / 'address.spvasm'
        source.write_text('''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %position %index
OpDecorate %position BuiltIn Position
OpDecorate %index BuiltIn VertexIndex
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%int = OpTypeInt 32 1
%float = OpTypeFloat 32
%vec = OpTypeVector %float 4
%input = OpTypePointer Input %int
%output = OpTypePointer Output %vec
%local = OpTypePointer Function %uint
%index = OpVariable %input Input
%position = OpVariable %output Output
%zero = OpConstant %uint 0
%two = OpConstant %uint 2
%three = OpConstant %uint 3
%stride = OpConstant %uint 48
%one = OpConstant %float 1
%pos = OpConstantComposite %vec %one %one %one %one
%main = OpFunction %void None %fn
%entry = OpLabel
%temp = OpVariable %local Function
%i = OpLoad %int %index
%u = OpBitcast %uint %i
%addr = OpIMul %uint %u %stride
OpStore %temp %addr
%reload = OpLoad %uint %temp
%words = OpShiftRightLogical %uint %reload %two
%component = OpBitwiseAnd %uint %words %three
OpStore %position %pos
OpReturn
OpFunctionEnd
''')
        original, folded = self.work / 'address.spv', self.work / 'folded.spv'
        subprocess.run(['spirv-as', '--target-env', 'spv1.3', str(source), '-o', str(original)], check=True)
        result = subprocess.run([str(self.probe), str(original), str(folded)], check=True, capture_output=True, text=True)
        self.assertEqual(result.stdout.strip(), '1')
        for path in (original, folded):
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(path)], check=True)


if __name__ == '__main__':
    unittest.main()
