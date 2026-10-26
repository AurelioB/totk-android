#!/usr/bin/env python3
"""Optimize an Android .spv/.vk/.vv seed into a new, validated directory.

No precision relaxation, specialization freezing, layout rewriting or debug
stripping: reflection is retained byte-for-byte after checking the interface.
An incomplete run deliberately has no seed-id.txt completion marker.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

import vk_reflect

FLAGS = ['-O', '--preserve-bindings', '--preserve-interface', '--preserve-spec-constants']


def tool_version(tool):
    return subprocess.check_output([tool, '--version'], stderr=subprocess.STDOUT, text=True).strip()


def instructions(data):
    if len(data) < 20 or len(data) % 4:
        raise ValueError('invalid SPIR-V size')
    words = struct.unpack(f'<{len(data) // 4}I', data)
    if words[0] != 0x07230203:
        raise ValueError('invalid SPIR-V magic')
    i = 5
    while i < len(words):
        n = words[i] >> 16
        if not n or i + n > len(words):
            raise ValueError('invalid SPIR-V instruction')
        yield words[i] & 0xffff, words[i + 1:i + n]
        i += n


def signature(data, stage):
    # reflect() needs translator metadata only for guest resource interpretation,
    # not these physical layout facts. Keep the original guest mapping in .vk.
    r = vk_reflect.reflect(data, {}, stage)
    binds = sorted(tuple(b[k] for k in ('binding', 'count', 'dtype', 'target', 'int', 'extent', 'name'))
                   for b in r['binds'])
    modes, specs = [], []
    ins = list(instructions(data))
    defs = {a[1]: (op, a) for op, a in ins if op in range(48, 53)}
    for op, a in ins:
        if op in (16, 331):
            modes.append((op, a[1:]))  # entry-point id is not a mode operand
        if op == 71 and a[1] == 1:
            defining = defs.get(a[0])
            if defining is None:
                raise ValueError('SpecId lacks a specialization constant')
            dop, da = defining
            # -O does not renumber retained spec constants; preserve the entire
            # expression/default, not merely the externally visible SpecId.
            specs.append((a[2], dop, da))
    return (r['vin'], r['fout'], r['depth'], r['local'], binds, sorted(modes), sorted(specs))


def check_reflection(data, text):
    records = [line.split() for line in text.splitlines() if line.strip()]
    stage = next(int(a[1]) for a in records if a[0] == 'stage')
    sp = vk_reflect.Spirv(data)
    if sp.exec_model != {0: 0, 1: 4, 5: 5}[stage]:
        raise ValueError('reflection stage mismatch')
    r = vk_reflect.reflect(data, {}, stage)
    expected = {b['binding']: b for b in r['binds']}
    seen = set()
    for a in records:
        if a[0] == 'b':
            values = list(map(int, a[1:]))
            b = expected[values[0]]
            for idx, key in ((1, 'count'), (2, 'dtype'), (8, 'target'), (9, 'int')):
                if values[idx] != b[key]:
                    raise ValueError(f'reflection binding {values[0]} {key} mismatch')
            if len(values) > 10 and values[10] != b['extent']:
                raise ValueError('reflection storage extent mismatch')
            seen.add(values[0])
        elif a[0] == 'local' and tuple(map(int, a[1:])) != r['local']:
            raise ValueError('reflection local size mismatch')
        elif a[0] == 'depth' and int(a[1]) != r['depth']:
            raise ValueError('reflection depth mismatch')
        elif a[0] == 'vin' and sorted(tuple(map(int, v.split(':'))) for v in a[1:]) != r['vin']:
            raise ValueError('reflection vertex input mismatch')
        elif a[0] == 'fout' and sorted(map(int, a[1:])) != r['fout']:
            raise ValueError('reflection fragment output mismatch')
    if seen != set(expected):
        raise ValueError('reflection descriptor list mismatch')
    return stage


def run(args):
    source, output = args.source.resolve(), args.output.resolve()
    if not source.is_dir():
        raise ValueError(f'seed does not exist: {source}')
    if source == output or source in output.parents or output in source.parents:
        raise ValueError('input and output directories must not overlap')
    if 'build' in output.parts:
        raise ValueError('output must not be inside a build directory')
    if output.exists():
        raise ValueError(f'output already exists (choose a fresh directory): {output}')
    for tool in (args.spirv_opt, args.spirv_val):
        subprocess.run([tool, '--version'], check=True, capture_output=True)
    excluded = set(getattr(args, 'exclude_key', []))
    if any(len(key) != 64 or any(c not in '0123456789abcdef' for c in key) for key in excluded):
        raise ValueError('--exclude-key requires a complete lowercase SHA-256 guest key')
    all_modules = sorted(source.glob('*.spv'))
    modules = [p for p in all_modules if p.stem.split('-')[0] not in excluded]
    omitted = [p.name for p in all_modules if p.stem.split('-')[0] in excluded]
    if not modules:
        raise ValueError('seed contains no SPIR-V modules')
    # Validate variant references before creating a partial output.
    for vv in source.glob('*.vv'):
        if vv.stem in excluded:
            continue
        for line in vv.read_text().splitlines():
            variant = line.split('\t')[0]
            if not variant or Path(variant).name != variant or not (source / (variant + '.spv')).is_file():
                raise ValueError(f'invalid/missing variant in {vv.name}: {variant}')
    output.mkdir(parents=True)

    def optimize(path):
        try:
            before = path.read_bytes()
            reflection = path.with_suffix('.vk').read_bytes()
            stage = check_reflection(before, reflection.decode())
            dest = output / path.name
            validation = subprocess.run([args.spirv_val, '--target-env', 'vulkan1.2', str(path)],
                                        capture_output=True, text=True)
            legalized = False
            opt_input = path
            if validation.returncode:
                if 'Capability StorageImageWriteWithoutFormat is required to write to storage image' not in validation.stderr:
                    raise ValueError(validation.stderr)
                # Known emitter omission: add only the declaration, no executable
                # changes. Validate again; a second diagnostic is never ignored.
                patched = before[:20] + struct.pack('<2I', (2 << 16) | 17, 56) + before[20:]
                dest.write_bytes(patched)
                subprocess.run([args.spirv_val, '--target-env', 'vulkan1.2', str(dest)],
                               check=True, capture_output=True, text=True)
                opt_input, legalized = dest, True
            subprocess.run([args.spirv_opt, '--target-env=vulkan1.2', *FLAGS, str(opt_input), '-o', str(dest)],
                           check=True, capture_output=True, text=True)
            subprocess.run([args.spirv_val, '--target-env', 'vulkan1.2', str(dest)],
                           check=True, capture_output=True, text=True)
            after = dest.read_bytes()
            if signature(before, stage) != signature(after, stage):
                raise ValueError('optimizer changed interface, execution modes or specialization constants')
            check_reflection(after, reflection.decode())
            dest.with_suffix('.vk').write_bytes(reflection)
            return dict(module=path.name, stage=stage, bytes_before=len(before), bytes_after=len(after),
                        instructions_before=sum(1 for _ in instructions(before)),
                        instructions_after=sum(1 for _ in instructions(after)),
                        sha256_before=hashlib.sha256(before).hexdigest(),
                        sha256_after=hashlib.sha256(after).hexdigest(),
                        reflection_sha256=hashlib.sha256(reflection).hexdigest(), validated=True,
                        added_storage_image_write_capability=legalized)
        except subprocess.CalledProcessError as e:
            raise ValueError(f'{path.name}: {e.cmd[0]}: {e.stderr}') from e
        except Exception as e:
            raise ValueError(f'{path.name}: {e}') from e

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        rows = list(pool.map(optimize, modules))
    for vv in sorted(source.glob('*.vv')):
        if vv.stem not in excluded:
            shutil.copyfile(vv, output / vv.name)
    with (output / 'optimization.csv').open('w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    identity = hashlib.sha256()
    for path in sorted(output.iterdir()):
        if path.suffix in ('.spv', '.vk', '.vv'):
            identity.update(path.name.encode() + b'\0' + hashlib.sha256(path.read_bytes()).digest())
    summary = dict(source=str(source), modules=len(rows), validated=len(rows), flags=FLAGS,
                   excluded_keys=sorted(excluded), excluded_modules=omitted,
                   spirv_opt=tool_version(args.spirv_opt),
                   spirv_val=tool_version(args.spirv_val),
                   seed_id=identity.hexdigest(),
                   legalized_storage_image_write_capability=sum(row['added_storage_image_write_capability'] for row in rows))
    for key in ('bytes_before', 'bytes_after', 'instructions_before', 'instructions_after'):
        summary[key] = sum(row[key] for row in rows)
    (output / 'optimization.json').write_text(json.dumps(summary, indent=2) + '\n')
    # Runtime incorporates this content identity in cache paths. Publish last.
    (output / 'seed-id.txt').write_text(identity.hexdigest() + '\n')
    print(json.dumps(summary, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--output', type=Path, default=Path('.scratch/seed-opt/v143'))
    parser.add_argument('--exclude-key', action='append', default=[],
                        help='explicitly omit a proven bogus guest key and its variants (recorded in report)')
    parser.add_argument('--jobs', type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument('--spirv-opt', default='spirv-opt')
    parser.add_argument('--spirv-val', default='spirv-val')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    try:
        run(args)
    except (ValueError, OSError, subprocess.CalledProcessError) as e:
        parser.exit(1, f'{e}\nNo completed seed published. Keep the failed directory for diagnosis.\n')


if __name__ == '__main__':
    main()
