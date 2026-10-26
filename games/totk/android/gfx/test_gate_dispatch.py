#!/usr/bin/env python3
"""Compare real runtime_dispatch to linear lookup and exercise both gate producers."""
import argparse
import ast
import os
from pathlib import Path
import shlex
import subprocess
from test_fps_auto_ownership import function


def gate_parser(path):
    tree = ast.parse(path.read_text())
    for loop in ast.walk(tree):
        if not isinstance(loop, ast.For):
            continue
        for i, node in enumerate(loop.body):
            if (isinstance(node, ast.Assign) and len(node.targets) == 1
                    and isinstance(node.targets[0], ast.Name) and node.targets[0].id == 'gates'):
                # Production: gates=[], TSV parser, sort, uniqueness assertion.
                return compile(ast.Module(body=loop.body[i:i + 4], type_ignores=[]), str(path), 'exec')
    raise AssertionError(f'gate parser block missing: {path}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scratch', type=Path, required=True)
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--gates-dir', type=Path)
    args = parser.parse_args()
    fixture = Path(__file__).resolve().with_suffix('.c')
    game = fixture.parents[2]
    scratch = args.scratch.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    parsers = [gate_parser(game / p) for p in ('scripts/prepare_runtime.py', 'android/fixture/make_fixture.py')]
    def parse(code, text):
        (scratch / 'probe.gates.tsv').write_text(text)
        scope = dict(REWRITTEN=scratch, out=scratch, name='probe')
        exec(code, scope)
        return scope['gates']
    for code in parsers:
        gates = parse(code, '0x30 svc 1 0x100\n0x10 cntpct 2 0x200\n0x20 cntvct 3 0x300\n')
        assert [g['ret'] for g in gates] == [16, 32, 48]
        assert [g['original'] for g in gates] == [512, 768, 256]
        assert parse(code, '') == []
        try:
            parse(code, '0x10 svc 1 0x100\n0x10 cntpct 2 0x200\n')
        except AssertionError as e:
            assert 'duplicate gate return address' in str(e)
        else:
            raise AssertionError('production generator accepted duplicate gates')
    data = []
    if args.gates_dir:
        for path in sorted(args.gates_dir.glob('*.gates.tsv')):
            text = path.read_text()
            rows = parse(parsers[0], text)
            assert rows == parse(parsers[1], text)
            data.append(rows)
        assert data, 'no gate inputs found'
    lines = []
    for i, gates in enumerate(data):
        lines.append(f'static const Gate input_{i}[] = {{')
        lines += [f'{{{g["ret"]}ULL,{g["original"]}ULL,{int(g["timer"])},{g["arg"]}}},' for g in gates] or ['{0,0,0,0},']
        lines.append('};')
    lines.append('static void inputs(void) {')
    for i, gates in enumerate(data):
        lines.append(f'check_input(input_{i},{len(gates)});')
    lines.append('}')
    (scratch / 'gate_inputs.inc').write_text('\n'.join(lines) + '\n')
    source = (game / 'runtime/loader.c').read_text()
    (scratch / 'gate_dispatch.inc').write_text(function(source, 'void runtime_dispatch(uint64_t ret,uint64_t *regs) {'))
    executable = scratch / 'gate_dispatch_test'
    subprocess.run(shlex.split(args.cc) + ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                   '-I', str(scratch), '-I', str(game / 'runtime'), str(fixture), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=60)
    print(f'PASS: both production generators sort/validate; {len(data)} supplied module tables')


if __name__ == '__main__':
    main()
