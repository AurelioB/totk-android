#!/usr/bin/env python3
"""check_baselines.py [--pin FILE]

Baselines are immutable. Verifies every docs/benchmarks/baseline-* file against its SHA-256 pinned in
docs/benchmarks/BASELINES.sha256 (sha256sum format), that no baseline file is missing or unpinned, and that
docs/benchmarks/CURRENT_BASELINE names a pinned baseline JSON. Exits 1 on any problem.
--pin FILE: pin a NEW baseline file (only when the user asked for a new baseline); an already pinned file is never
re-pinned."""
import argparse, hashlib, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from benchlib import BENCH_DOCS, CURRENT_BASELINE, REPO  # noqa: E402

PINS = BENCH_DOCS / 'BASELINES.sha256'


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_pins():
    pins = {}
    for line in PINS.read_text().splitlines():
        if line.strip():
            digest, name = line.split(None, 1)
            pins[name.lstrip('*')] = digest
    return pins


def problems():
    out, pins = [], read_pins()
    for name, digest in pins.items():
        p = BENCH_DOCS / name
        if not p.exists():
            out.append(f'{name}: pinned baseline is missing')
        elif sha256(p) != digest:
            out.append(f'{name}: CHANGED (sha256 {sha256(p)}, pinned {digest}); baselines are immutable, restore it')
    for p in sorted(BENCH_DOCS.glob('baseline-*')):
        if p.name not in pins:
            out.append(f'{p.name}: baseline file not pinned in {PINS.name} (tools/bench/check_baselines.py --pin)')
    cur = CURRENT_BASELINE.read_text().strip() if CURRENT_BASELINE.exists() else ''
    cur_path = REPO / cur
    if not cur or cur_path.parent != BENCH_DOCS or cur_path.name not in pins or not cur.endswith('.json'):
        out.append(f'{CURRENT_BASELINE.name}: {cur!r} is not a pinned docs/benchmarks/baseline-*.json')
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--pin', type=Path, help='pin a new docs/benchmarks/baseline-* file')
    a = ap.parse_args()
    if a.pin:
        p = a.pin.resolve()
        if p.parent != BENCH_DOCS.resolve() or not p.name.startswith('baseline-') or not p.is_file():
            sys.exit(f'--pin: {a.pin} is not an existing docs/benchmarks/baseline-* file')
        if p.name in read_pins():
            sys.exit(f'--pin: {p.name} is already pinned; baselines are never re-pinned')
        with open(PINS, 'a') as f:
            f.write(f'{sha256(p)}  {p.name}\n')
        print(f'pinned {p.name}')
    bad = problems()
    for b in bad:
        print(f'check_baselines: {b}', file=sys.stderr)
    if not bad:
        print(f'check_baselines: {len(read_pins())} baseline files match their pins; current {CURRENT_BASELINE.read_text().strip()}')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
