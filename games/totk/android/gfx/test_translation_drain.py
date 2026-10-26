#!/usr/bin/env python3
"""Exercise production translation-worker self-drain without a GPU or game files."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
from test_fps_auto_ownership import function, section


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scratch', type=Path, required=True)
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    args = parser.parse_args()
    fixture = Path(__file__).resolve().with_suffix('.c')
    game = fixture.parents[2]
    source = (game / 'third_party/d2r_gfx/gfx/nvn/hle_nvn.c').read_text()
    production = section(source, 'static _Thread_local int t_on_x_thread;', 'static uint64_t x_push(')
    production += section(source, 'static void x_wait_job(uint64_t job) {', '// Fence token for FenceSync:')
    scratch = args.scratch.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    (scratch / 'translation_drain.inc').write_text(production)
    executable = scratch / 'translation_drain_test'
    subprocess.run(shlex.split(args.cc) + ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                   '-pthread', '-I', str(scratch), str(fixture), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], cwd=scratch, check=True, timeout=10)


if __name__ == '__main__':
    main()
