#!/usr/bin/env python3
"""Compile and run the synthetic early-fragment Vulkan host fixture, without game data.

From the worktree root:
    python3 games/totk/android/gfx/test_vkearlyz.py
    python3 games/totk/android/gfx/test_vkearlyz.py --icd /path/to/lvp_icd.x86_64.json

Requires a host C compiler, Vulkan development headers/loader (-lvulkan),
glslangValidator, and a Vulkan ICD supporting fragmentStoresAndAtomics and
D32_SFLOAT attachment readback. Mesa Lavapipe is selected automatically when
installed and no ICD environment override is present. No window system is used.
All generated shaders and the executable stay under .scratch/earlyz.
"""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess


VERTEX = """#version 450
layout(push_constant) uniform Draw { float raster_depth; } draw;
void main() {
    vec2 vertices[3] = vec2[3](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));
    gl_Position = vec4(vertices[gl_VertexIndex], draw.raster_depth, 1);
}
"""
FRAGMENT = """#version 450
{early}
layout(set=0, binding=0, std430) buffer Result {{ uint hits[64]; }} result;
layout(location=0) out vec4 color;
uint pixel() {{ return uint(gl_FragCoord.y) * 8u + uint(gl_FragCoord.x); }}
void main() {{
    atomicAdd(result.hits[pixel()], 1u);
    {body}
}}
"""
SHADERS = (
    ("fullscreen.vert", VERTEX),
    ("store.frag", FRAGMENT.format(
        early="layout(early_fragment_tests) in;", body="color = vec4(1, 0, 0, 1);")),
    ("discard.frag", FRAGMENT.format(
        early="layout(early_fragment_tests) in;",
        body="if ((uint(gl_FragCoord.x) & 1u) == 0u) discard; color = vec4(0, 1, 0, 1);")),
    ("depth.frag", FRAGMENT.format(
        early="", body="gl_FragDepth = 0.25; color = vec4(0, 0, 1, 1);")),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--icd", type=Path, help="Vulkan ICD manifest; overrides VK_DRIVER_FILES and VK_ICD_FILENAMES")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"), help="host C compiler command (default: CC or cc)")
    parser.add_argument("--scratch", type=Path, help="output directory inside this worktree's .scratch/earlyz")
    args = parser.parse_args()
    source = Path(__file__).resolve().with_suffix(".c")
    root = source.parents[4]
    scratch_root = root / ".scratch" / "earlyz"
    scratch = (args.scratch or scratch_root / "pixel-fixture").resolve()
    if not scratch.is_relative_to(scratch_root.resolve()):
        parser.error("--scratch must be inside this worktree's .scratch/earlyz")
    compiler = shlex.split(args.cc)
    if not compiler or not shutil.which(compiler[0]):
        parser.error(f"host C compiler not found: {args.cc}")
    glslang = shutil.which("glslangValidator")
    if not glslang:
        parser.error("glslangValidator is required (typically the glslang-tools package)")
    environment = os.environ.copy()
    icd = args.icd
    if icd is None and not any(environment.get(key) for key in ("VK_DRIVER_FILES", "VK_ICD_FILENAMES")):
        candidates = sorted(Path("/usr/share/vulkan/icd.d").glob("lvp_icd*.json"))
        if candidates:
            icd = candidates[0]
    if icd is not None:
        icd = icd.resolve()
        if not icd.is_file():
            parser.error(f"ICD manifest not found: {icd}")
        environment["VK_DRIVER_FILES"] = str(icd)
        environment["VK_ICD_FILENAMES"] = str(icd)
        print(f"Vulkan ICD: {icd}", flush=True)
    scratch.mkdir(parents=True, exist_ok=True)
    modules = []
    for name, text in SHADERS:
        glsl = scratch / name
        spirv = scratch / f"{name}.spv"
        glsl.write_text(text)
        subprocess.run([glslang, "-V", "--target-env", "vulkan1.1", str(glsl), "-o", str(spirv)], check=True)
        modules.append(str(spirv))
    executable = scratch / "vkearlyztest"
    subprocess.run(compiler + ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(executable), "-lvulkan"], check=True)
    subprocess.run([str(executable), *modules], env=environment, cwd=scratch, check=True)


if __name__ == "__main__":
    main()
