#!/usr/bin/env python3
"""abtest.py <ab.json> [--ref A] [--out FILE]

Per device x scene of an in-session A/B (ab.sh, then analyze.py <out> --arms --json <ab.json>): every arm against the
reference arm (--ref, default A), per metric, with a verdict.

Values: each arm's pooled summary over all of its windows (fps mean, fps min, p50 median, p95 max, worst frame, GPU ms
mean; the >50 ms and long >=45 ms counts per window). Noise: for each arm, the spread (max - min) of the same metric
over that arm's blocks (each block is one application of the arm in the ABBA order); the noise of a comparison is the
larger of the two arms' spreads.

Verdict rule for one metric, delta = arm - reference (per window for counts):
  same            |delta| <= the compare.py tolerance of the metric (compare.py --help)
  within noise    |delta| > tolerance but <= noise: the arm's own blocks differ that much between repeats
  better / worse  |delta| > tolerance and > noise, by the metric's direction
  better? worse?  |delta| > tolerance, but an arm has fewer than two blocks: no noise estimate (unconfirmed)
  n/a             a value is missing
An arm is worse if any primary metric (fps mean, p50 median, p95 max, GPU ms mean, >50 ms per window) is worse, else
better if any is better, else inconclusive if any is better? / worse?, else same. Exit 1 when any arm is worse."""
import argparse, io, json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from benchlib import METRICS, PRIMARY, guard_output  # noqa: E402
from analyze import AB_METRICS, block_metric  # noqa: E402


def metric_verdict(key, ref, arm):
    """(delta, tolerance, noise, verdict) for one metric of two arm records ({'summary', 'spread'})."""
    _, _, direction, kind, (tol_abs, tol_rel) = next(m for m in METRICS if m[0] == key)
    r, a = block_metric(ref['summary'], key), block_metric(arm['summary'], key)
    if r is None or a is None:
        return None, None, None, 'n/a'
    d = a - r
    tol = tol_abs if kind == 'abs' else max(tol_abs, tol_rel * abs(r))
    spreads = (ref['spread'].get(key), arm['spread'].get(key))
    noise = None if None in spreads else max(spreads)
    if abs(d) <= tol:
        return d, tol, noise, 'same'
    word = 'better' if d * direction > 0 else 'worse'
    if noise is None:
        return d, tol, noise, word + '?'
    return d, tol, noise, 'within noise' if abs(d) <= noise else word


def arm_verdict(per):
    prim = [per[k][3] for k in PRIMARY if k in per]
    if 'worse' in prim:
        return 'worse'
    if 'better' in prim:
        return 'better'
    if any(v.endswith('?') for v in prim):
        return 'inconclusive'
    return 'n/a' if all(v == 'n/a' for v in prim) else 'same'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], epilog=__doc__.split('\n\n', 1)[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('result', help='analyze.py --arms JSON')
    ap.add_argument('--ref', default='A', help='reference arm (default A)')
    ap.add_argument('--out', help='also write the report to this file')
    a = ap.parse_args()
    if a.out:
        guard_output(a.out)
    doc = json.load(open(a.result, encoding='utf-8'))
    buf, worse = io.StringIO(), 0
    p = lambda *x: print(*x, file=buf)
    fmt = lambda v, nd=2: '-' if v is None else f'{v:+.{nd}f}' if isinstance(v, float) else str(v)
    runs = [r for r in doc.get('runs', []) if r.get('ab')]
    if not runs:
        sys.exit(f'{a.result}: no run has "ab" (run analyze.py with --arms)')
    for run in runs:
        ab = run['ab']; arms = ab['arms']; c = ab['windows']
        p(f'## {run["device"]} / {run["scene"]}: {c["assigned"]} windows in arms, {c["straddling"]} straddling and '
          f'{c["unlabelled"]} unlabelled dropped')
        if a.ref not in arms:
            p(f'  no reference arm {a.ref} (arms: {", ".join(arms) or "none"})')
            continue
        ref = arms[a.ref]
        for name, arm in arms.items():
            if name == a.ref:
                continue
            per = {k: metric_verdict(k, ref, arm) for k in AB_METRICS}
            v = arm_verdict(per)
            worse += v == 'worse'
            p(f'\n  arm {name} vs {a.ref}: {v.upper()}  ({len(arm["blocks"])} vs {len(ref["blocks"])} blocks, '
              f'{arm["summary"]["windows"]} vs {ref["summary"]["windows"]} windows)')
            p(f'  {"metric":<16} {a.ref:>9} {name:>9} {"delta":>9} {"tolerance":>9} {"noise":>9}  verdict')
            for key, label, *_ in METRICS:
                if key not in per:
                    continue
                d, tol, noise, verdict = per[key]
                rate = key in ('over50_total', 'long_ge45_total')
                rv, av = block_metric(ref['summary'], key), block_metric(arm['summary'], key)
                p(f'  {label + (" /win" if rate else ""):<16} {"-" if rv is None else f"{rv:g}":>9} '
                  f'{"-" if av is None else f"{av:g}":>9} {fmt(d):>9} {"-" if tol is None else f"{tol:.2f}":>9} '
                  f'{"-" if noise is None else f"{noise:.2f}":>9}  {verdict}{" *" if key in PRIMARY else ""}')
        p('\n  * primary metric. Rule: same within the compare.py tolerance; better/worse only beyond both the tolerance '
          'and the same-arm block spread (noise); "?" = no noise estimate (an arm ran one block).')
    sys.stdout.write(buf.getvalue())
    if a.out:
        with open(a.out, 'w', encoding='utf-8') as f:
            f.write(buf.getvalue())
    sys.exit(1 if worse else 0)


if __name__ == '__main__':
    main()
