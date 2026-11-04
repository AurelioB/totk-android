#!/usr/bin/env python3
"""check_baselines.py

Baselines are immutable. Verifies every baseline file (docs/benchmarks/baseline-* and docs/benchmarks/baselines/**/*.json
except index.json) against its SHA-256 pinned in docs/benchmarks/BASELINES.sha256 (sha256sum format, paths relative to
docs/benchmarks), that no baseline file is missing or unpinned, and that every entry of docs/benchmarks/baselines/index.json
names a pinned baseline file holding that device, with a signature that matches its fields and is unique.
Exits 1 on any problem. Pinning happens only in `ledger.py add` (a new frequency signature's baseline)."""
import argparse, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from benchlib import (BENCH_DOCS, index_path, load_index, load_result, pins_path, read_pins, sha256_file,  # noqa: E402
                      signature_of_fields)


def baseline_files(docs):
    files = [p for p in docs.glob('baseline-*') if p.is_file()]
    files += [p for p in (docs / 'baselines').rglob('*.json') if p != index_path(docs)]
    return sorted(p.relative_to(docs).as_posix() for p in files)


def problems(docs=None):
    docs = docs or BENCH_DOCS
    out, pins = [], read_pins(docs)
    for name, digest in pins.items():
        p = docs / name
        if not p.exists():
            out.append(f'{name}: pinned baseline is missing')
        elif sha256_file(p) != digest:
            out.append(f'{name}: CHANGED (sha256 {sha256_file(p)}, pinned {digest}); baselines are immutable, restore it')
    for name in baseline_files(docs):
        if name not in pins:
            out.append(f'{name}: baseline file not pinned in {pins_path(docs).name}')
    seen = set()
    for b in load_index(docs)['baselines']:
        sig = b.get('signature')
        if sig in seen:
            out.append(f'index: signature {sig} listed twice')
        seen.add(sig)
        if signature_of_fields(b) != sig or b.get('short') != sig[:12]:
            out.append(f'index: {b.get("file")} / {b.get("device")}: signature does not match its fields')
        if b.get('file') not in pins:
            out.append(f'index: {b.get("file")} is not a pinned baseline file')
        elif (docs / b['file']).exists() and b.get('device') not in load_result(docs / b['file'])['devices']:
            out.append(f'index: {b["file"]} has no device {b.get("device")!r}')
    return out


def main():
    argparse.ArgumentParser(description=__doc__.split('\n\n')[0]).parse_args()
    bad = problems()
    for b in bad:
        print(f'check_baselines: {b}', file=sys.stderr)
    if not bad:
        print(f'check_baselines: {len(read_pins())} baseline files match their pins; '
              f'{len(load_index()["baselines"])} frequency signatures in {index_path().relative_to(BENCH_DOCS.parent.parent)}')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
