#!/usr/bin/env python3
"""Actual locked Vulkan submission + retained-present plan identity vs universal3.
Run under the assigned CPU affinity, with a fresh scratch directory argument.
Completed GPU timestamp values are fixed by the C probe, so host contention
cannot change admission. This does not measure Android scanout or AFME cost.
"""
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

REFERENCE = 'c2af5eeb'
ROOT = Path(__file__).resolve().parents[1]

def main():
    scratch = Path(sys.argv[1]).resolve()
    scratch.mkdir(parents=True, exist_ok=False)
    reference = scratch / 'reference'
    reference.mkdir()
    archive = subprocess.check_output(['git', 'archive', REFERENCE, 'games/totk', 'tests'], cwd=ROOT)
    with tarfile.open(fileobj=io.BytesIO(archive)) as source:
        source.extractall(reference, filter='data')
    shutil.copyfile(ROOT / 'tests/framegen_graph_probe.c', reference / 'tests/framegen_graph_probe.c')
    fragment = scratch / 'graph.mk'
    fragment.write_text('''graph: $(OUT)/graph
$(OUT)/graph: $(TOTK)/../../tests/framegen_graph_probe.c $(VKTEST_SRC)
\t@mkdir -p $(dir $@)
\t$(CC) $(VKTEST_FLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $(filter %.c,$^) $(VKTEST_LIBS)
''')
    env = os.environ.copy()
    for key in ('TOTK_FRAME_LAG', 'TOTK_VK_REC_THREADS', 'TOTK_FRAMEGEN_ENGINE', 'TOTK_FRAMEGEN_DIAG', 'TOTK_PRESENT_TIMELINE'):
        env.pop(key, None)
    env.update(PYTHON_CPU_COUNT='4', LP_NUM_THREADS='0', VK_ICD_FILENAMES='/usr/share/vulkan/icd.d/lvp_icd.json')
    binaries = {}
    for name, tree in (('reference', reference), ('current', ROOT)):
        out = scratch / (name + '-out')
        with (scratch / (name + '-build.log')).open('wb') as log:
            subprocess.run(['make', '-C', str(tree / 'games/totk/android'), '-f', 'Makefile', '-f', str(fragment), '-j4', '-s',
                            'graph', 'TARGET=linux', 'CC=clang -DTOTK_GAME_V143=1', 'OUT=' + str(out)], env=env,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        binaries[name] = out / 'graph'
    for arm in ('default', 'protected'):
        e = env.copy()
        if arm == 'protected':
            e.update(TOTK_FRAME_LAG='1', TOTK_VK_REC_THREADS='2')
        golden = None
        for name, engine, argument in (('reference', None, None), ('current', None, None),
                                      ('current', '', None), ('current', 'camera', None),
                                      ('current', 'qcom', None), ('current', 'lsfg', None),
                                      ('current', 'qcom', '--after-refusal'), ('current', 'qcom', '--after-cooldown'),
                                      ('current', 'lsfg', '--after-refusal'), ('current', 'lsfg', '--after-cooldown')):
            case = '-'.join((arm, name, 'unset' if engine is None else engine or 'empty',
                             argument.removeprefix('--') if argument else 'fresh'))
            work = scratch / case
            work.mkdir()
            if engine is None:
                e.pop('TOTK_FRAMEGEN_ENGINE', None)
            else:
                e['TOTK_FRAMEGEN_ENGINE'] = engine
            command = [str(binaries[name])] + ([argument] if argument else [])
            e.pop('TOTK_PRESENT_TIMELINE', None)
            result = subprocess.run(command, cwd=work, env=e, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=180, check=False)
            (scratch / (case + '.log')).write_bytes(result.stdout)
            lines = [line for line in result.stdout.decode(errors='replace').splitlines() if line.startswith('GRAPH ')]
            if result.returncode or not lines:
                raise RuntimeError(f'{case}: exit={result.returncode}; see saved log')
            if golden is None:
                golden = lines
            elif lines != golden:
                for index, (before, after) in enumerate(zip(golden, lines)):
                    if before != after:
                        raise AssertionError(f'{case}: graph differs at {index}:\n{before}\n{after}')
                raise AssertionError(f'{case}: graph lengths differ: {len(golden)} vs {len(lines)}')
            print(f'{case}: identical to {REFERENCE[:7]} ({len(lines)} graph records); {lines[-1]}', flush=True)
        # Host admission is unavailable: explicit off and requested-but-unavailable
        # must preserve the same graph, not just the default-unset path.
        for timeline in ('0', '1'):
            e.pop('TOTK_FRAMEGEN_ENGINE', None)
            e['TOTK_PRESENT_TIMELINE'] = timeline
            case = arm + '-current-timeline-' + timeline
            work = scratch / case
            work.mkdir()
            result = subprocess.run([str(binaries['current'])], cwd=work, env=e,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180, check=False)
            (scratch / (case + '.log')).write_bytes(result.stdout)
            lines = [line for line in result.stdout.decode(errors='replace').splitlines() if line.startswith('GRAPH ')]
            if result.returncode or lines != golden:
                raise AssertionError(f'{case}: exit={result.returncode}, graph differs; see saved log')
            print(f'{case}: identical to {REFERENCE[:7]} ({len(lines)} graph records); {lines[-1]}', flush=True)
    print('PASS locked graph: universal3, unset/empty/camera, host-refused external engines and retirement, timeline off/unavailable; both arms')

if __name__ == '__main__':
    main()
