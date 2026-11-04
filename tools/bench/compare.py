#!/usr/bin/env python3
"""compare.py <reference.json> <new.json> [--out FILE]

Per device x scene, the reference's and the new run's summary metrics (totk-android-benchmark/1 results: a baseline
from docs/benchmarks/baselines/index.json, a ledger run file or an analyze.py --json output) with deltas and a verdict
per metric and per scene. Devices are paired by label, else by model / name. Exits 1 when any scene whose frequency
signatures match is worse.

Frequency signature (benchlib.signature_of): device model + SoC, the settings_env hash, the per-cluster effective CPU
caps (cpufreq scaling_max_freq, kHz) and the effective GPU cap (Hz). Each device pair is labelled:
  match           same signature: the verdicts are a regression check (they decide the exit status)
  reference only  the reference has no GPU cap recorded (the 1.0.0 baseline) but model, settings and CPU caps match:
                  shown for information, never decides the exit status
  NOT COMPARABLE  the signatures differ, or one side has none: printed with a loud WARNING, never decides the exit
                  status. Compare against the baseline of the new run's own signature instead.
The signature of baseline-1.0.0.json comes from the baseline index (CPU caps from its sampler records, GPU cap null).

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
of them is better, else 'same'. A WARNING also marks devices whose settings_env or cpufreq caps differ, or whose
frequency limits changed during a run (freq_stable false)."""
import argparse, io, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from benchlib import compare_docs, guard_output, load_result  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], epilog=__doc__.split('\n\n', 1)[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('baseline', help='the reference result JSON')
    ap.add_argument('new')
    ap.add_argument('--out', help='also write the comparison to this file')
    a = ap.parse_args()
    if a.out:
        guard_output(a.out)
    base, new = load_result(a.baseline), load_result(a.new)
    buf = io.StringIO()
    print(f'# {new.get("name")} vs {base.get("name")}', file=buf)
    verdicts = compare_docs(base, new, buf, base_path=a.baseline, new_path=a.new)
    count = lambda v, rel: sum(1 for o, r in verdicts.values() if o == v and r == rel)
    print(f'\nscenes with matching signatures: {count("better", "match")} better, {count("worse", "match")} worse, '
          f'{count("same", "match")} same' + (f', {count("n/a", "match")} n/a' if count('n/a', 'match') else ''), file=buf)
    other = {r: sum(1 for _, x in verdicts.values() if x == r) for r in ('reference-only', 'mismatch', 'unknown')}
    if any(other.values()):
        print(f'scenes not used for the verdict: {other["reference-only"]} reference only, '
              f'{other["mismatch"] + other["unknown"]} NOT COMPARABLE', file=buf)
    sys.stdout.write(buf.getvalue())
    if a.out:
        with open(a.out, 'w', encoding='utf-8') as f:
            f.write(buf.getvalue())
    sys.exit(1 if count('worse', 'match') else 0)


if __name__ == '__main__':
    main()
