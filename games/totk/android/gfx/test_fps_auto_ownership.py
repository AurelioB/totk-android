#!/usr/bin/env python3
"""Run the real NVN acquire boundary and frontend FPSauto observation mapping.

Only Android/GPU services are replaced by deterministic host fixture services.
The acquire function, replay-job wait, observation state and runtime functions
are extracted verbatim from production sources; no policy/mapping mirror is used.
Requires a host C compiler and pthreads. All generated files stay in --scratch.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess


def section(source, start, end):
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError(f"production section anchors changed: {start!r}, {end!r}")
    begin = source.index(start)
    finish = source.index(end, begin)
    return source[begin:finish]


def function(source, start):
    if source.count(start) != 1:
        raise ValueError(f"production function anchor changed: {start!r}")
    begin = source.index(start)
    # Production nested blocks are indented; the function's closer is not.
    finish = source.index("\n}", begin) + 2
    return source[begin:finish] + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scratch", type=Path, required=True,
                        help="directory for extracted production code and host executable")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    fixture = Path(__file__).resolve().with_suffix(".c")
    game = fixture.parents[2]
    frontend = (game / "runtime/frontend_android.c").read_text()
    nvn = (game / "third_party/d2r_gfx/gfx/nvn/hle_nvn.c").read_text()
    runtime = section(frontend, "static _Atomic int fps_auto_on;",
                      "static void fps_auto_configure(void)")
    runtime += section(frontend, "unsigned runtime_android_fps_auto_acquire(",
                       "int runtime_android_display_pace_active(void)")
    scratch = args.scratch.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    (scratch / "fps_auto_frontend.inc").write_text(runtime)
    (scratch / "fps_auto_x_wait.inc").write_text(function(nvn, "static void x_wait_job(uint64_t job) {"))
    (scratch / "fps_auto_acquire.inc").write_text(function(nvn, "NVN(nvnWindowAcquireTexture,"))
    (scratch / "fps_hybrid_acquire.inc").write_text(function(nvn, "static unsigned hybrid_acquire(unsigned guest) {"))
    executable = scratch / "fps_auto_ownership_test"
    subprocess.run(shlex.split(args.cc) + ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-DGPU_VK", "-DTOTK_VK_TESTS",
                   "-pthread", "-I", str(scratch), "-I", str(game / "runtime"),
                   "-I", str(fixture.parent), str(fixture),
                   str(game / "runtime/fps_hybrid_runtime.c"), "-lm", "-o", str(executable)], check=True)
    subprocess.run([str(executable)], cwd=scratch, check=True, timeout=30)
    subprocess.run([str(executable), "--hybrid"], cwd=scratch, check=True, timeout=30)


if __name__ == "__main__":
    main()
