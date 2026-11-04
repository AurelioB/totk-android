#!/usr/bin/env python3
"""compare.py <baseline.json> <new.json> [--out FILE] [--new-baseline]

Per device x scene, the baseline's and the new run's summary metrics (totk-android-benchmark/1 results, e.g.
docs/benchmarks/baseline-1.0.0.json and an analyze.py --json output) with deltas and a verdict per metric and per scene.
Devices are paired by label, else by model / name. Exits 1 when any scene is worse.

Verdict thresholds (a difference within the tolerance is 'same'; beyond it 'better' or 'worse'):
  fps mean        max(0.5 fps, 2 %)        higher is better
  fps min         max(1 fps, 5 %)          higher is better
  p50 median ms   max(0.3 ms, 2 %)         lower is better
  p95 max ms      max(1 ms, 5 %)           lower is better
  worst frame ms  max(5 ms, 15 %)          lower is better
  GPU ms mean     max(0.5 ms, 3 %)         lower is better
  long >=45 ms    per window: max(0.5, 25 %) lower is better (totals over runs with different window counts)
  >50 ms frames   per window: max(0.5, 25 %) lower is better
  CPU / GPU max C 3 C                      lower is better
A scene is 'worse' if any of fps mean, p50 median, p95 max, GPU ms mean or >50 ms frames is worse, else 'better' if any
of them is better, else 'same'. A WARNING marks devices whose settings_env or cpufreq caps (scaling_max < cpuinfo_max,
e.g. the Odin 3's non-thermal CPU cap) differ between the runs: those results are not comparable."""
import argparse, io, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from benchlib import compare_docs, guard_output, load_result  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], epilog=__doc__.split('\n\n', 2)[2],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('baseline')
    ap.add_argument('new')
    ap.add_argument('--out', help='also write the comparison to this file')
    ap.add_argument('--new-baseline', action='store_true', help=argparse.SUPPRESS)
    a = ap.parse_args()
    if a.out:
        guard_output(a.out, a.new_baseline)
    base, new = load_result(a.baseline), load_result(a.new)
    buf = io.StringIO()
    print(f'# {new.get("name")} vs {base.get("name")}', file=buf)
    verdicts = compare_docs(base, new, buf)
    counts = {v: sum(1 for x in verdicts.values() if x == v) for v in ('better', 'worse', 'same', 'n/a')}
    print(f'\nscenes: {counts["better"]} better, {counts["worse"]} worse, {counts["same"]} same'
          + (f', {counts["n/a"]} n/a' if counts['n/a'] else ''), file=buf)
    sys.stdout.write(buf.getvalue())
    if a.out:
        with open(a.out, 'w', encoding='utf-8') as f:
            f.write(buf.getvalue())
    sys.exit(1 if counts['worse'] else 0)


if __name__ == '__main__':
    main()
