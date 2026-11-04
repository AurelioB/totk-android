"""Shared helpers of the benchmark tools: repository paths, the baseline write guard and the comparison metrics."""
import json, sys
from pathlib import Path

SCHEMA = 'totk-android-benchmark/1'
REPO = Path(__file__).resolve().parents[2]
BENCH_DOCS = REPO / 'docs/benchmarks'
CURRENT_BASELINE = BENCH_DOCS / 'CURRENT_BASELINE'   # one line: the current baseline JSON, relative to the repo


def is_baseline_path(path):
    p = Path(path).resolve()
    return p.parent == BENCH_DOCS.resolve() and p.name.startswith('baseline-')


def guard_output(path, new_baseline=False):
    """Baselines are immutable: refuse to write docs/benchmarks/baseline-* unless --new-baseline was given, and even then
    never overwrite an existing file."""
    if is_baseline_path(path):
        if not new_baseline:
            sys.exit(f'refusing to write {path}: docs/benchmarks/baseline-* files are immutable '
                     '(record a new baseline only when asked, with --new-baseline and a new file name)')
        if Path(path).exists():
            sys.exit(f'refusing to overwrite the existing baseline {path}: a new baseline needs a new file name')
    elif new_baseline:
        sys.exit(f'--new-baseline: {path} is not a docs/benchmarks/baseline-<name>.json path')


def current_baseline():
    """The current baseline JSON path (absolute), from docs/benchmarks/CURRENT_BASELINE."""
    rel = CURRENT_BASELINE.read_text().strip()
    return REPO / rel


def load_result(path):
    with open(path, encoding='utf-8') as f:
        doc = json.load(f)
    if doc.get('schema') != SCHEMA:
        sys.exit(f'{path}: schema {doc.get("schema")!r}, expected {SCHEMA!r}')
    return doc


# Comparison metrics: (summary key, label, direction (+1 higher is better, -1 lower is better), kind, tolerance).
# kind 'abs_rel': same while |delta| <= max(abs, rel * |baseline|); kind 'rate': the value is a total over the windows,
# compared per window, same while |delta per window| <= max(abs, rel * baseline per window); kind 'abs': |delta| <= abs.
# A difference beyond the tolerance is 'better' or 'worse' by the direction. PRIMARY metrics decide a scene's verdict:
# worse if any primary metric is worse, else better if any is better, else same.
METRICS = [
    ('fps_mean',        'fps mean',        +1, 'abs_rel', (0.5, 0.02)),
    ('fps_min',         'fps min',         +1, 'abs_rel', (1.0, 0.05)),
    ('p50_ms_median',   'p50 median ms',   -1, 'abs_rel', (0.3, 0.02)),
    ('p95_ms_max',      'p95 max ms',      -1, 'abs_rel', (1.0, 0.05)),
    ('max_ms',          'worst frame ms',  -1, 'abs_rel', (5.0, 0.15)),
    ('gpu_ms_mean',     'GPU ms mean',     -1, 'abs_rel', (0.5, 0.03)),
    ('long_ge45_total', 'long >=45 ms',    -1, 'rate',    (0.5, 0.25)),
    ('over50_total',    '>50 ms frames',   -1, 'rate',    (0.5, 0.25)),
    ('cpu_max_c',       'CPU max C',       -1, 'abs',     (3.0, 0)),
    ('gpu_max_c',       'GPU max C',       -1, 'abs',     (3.0, 0)),
]
PRIMARY = ('fps_mean', 'p50_ms_median', 'p95_ms_max', 'gpu_ms_mean', 'over50_total')


def metric_verdict(key, base, new):
    """(delta, verdict) for one summary metric of two runs' summaries."""
    _, _, direction, kind, (tol_abs, tol_rel) = next(m for m in METRICS if m[0] == key)
    b, n = base.get(key), new.get(key)
    if b is None or n is None:
        return None, 'n/a'
    delta = n - b
    if kind == 'rate':
        bw, nw = base.get('windows') or 0, new.get('windows') or 0
        if not bw or not nw:
            return delta, 'n/a'
        b, n = b / bw, n / nw
    d = n - b
    tol = tol_abs if kind == 'abs' else max(tol_abs, tol_rel * abs(b))
    if abs(d) <= tol:
        return delta, 'same'
    return delta, 'better' if d * direction > 0 else 'worse'


def scene_verdicts(base, new):
    """{metric: (delta, verdict)} and the scene's overall verdict for two runs' summaries."""
    per = {m[0]: metric_verdict(m[0], base, new) for m in METRICS}
    prim = [per[k][1] for k in PRIMARY]
    overall = 'worse' if 'worse' in prim else 'better' if 'better' in prim else 'same'
    if all(v == 'n/a' for v in prim):
        overall = 'n/a'
    return per, overall


def caps_of(run):
    """The set of cpufreq limits (scaling_max < cpuinfo_max) seen during a run's pans."""
    return sorted({(c['scaling_max'], c['cpuinfo_max']) for t in run.get('thermal', []) for c in t['cpufreq_caps_khz']})


def match_device(base_doc, label, dev):
    """The baseline device label matching a device: the same label, or the same model / name."""
    devices = base_doc.get('devices', {})
    if label in devices:
        return label
    names = {(dev.get('model') or dev.get('name') or '').lower()} - {''}
    for k, v in devices.items():
        if names & {(v.get('model') or '').lower(), (v.get('name') or '').lower()}:
            return k
    return None


def comparability(base_doc, base_label, new_doc, new_label):
    """Warnings that make two runs of one device not comparable: settings.env and cpufreq caps differences."""
    warns = []
    be = base_doc['devices'].get(base_label, {}).get('settings_env')
    ne = new_doc['devices'].get(new_label, {}).get('settings_env')
    if be is not None and ne is not None and be != ne:
        diff = sorted(k for k in set(be) | set(ne) if be.get(k) != ne.get(k))
        warns.append(f'settings_env differs ({", ".join(f"{k}: {be.get(k)} -> {ne.get(k)}" for k in diff)})')
    bruns = {r['scene']: r for r in base_doc['runs'] if r['device'] == base_label}
    for r in new_doc['runs']:
        if r['device'] != new_label or r['scene'] not in bruns:
            continue
        bc, nc = caps_of(bruns[r['scene']]), caps_of(r)
        if bc != nc:
            fmt = lambda caps: ', '.join(f'{s}/{c}' for s, c in caps) or 'none'
            warns.append(f'{r["scene"]}: cpufreq caps differ (scaling_max/cpuinfo_max kHz {fmt(bc)} -> {fmt(nc)})')
    return warns


def fmt(v, nd=2):
    if v is None:
        return '-'
    if isinstance(v, float):
        return f'{v:.{nd}f}'
    return str(v)


def compare_docs(base_doc, new_doc, out=sys.stdout):
    """Print the per device x scene comparison of two result documents; returns {(new_label, scene): overall}."""
    verdicts = {}
    for new_label, dev in new_doc['devices'].items():
        base_label = match_device(base_doc, new_label, dev)
        if base_label is None:
            print(f'\n## {new_label}: not in the reference run, nothing to compare', file=out)
            continue
        warns = comparability(base_doc, base_label, new_doc, new_label)
        bruns = {r['scene']: r for r in base_doc['runs'] if r['device'] == base_label}
        for run in (r for r in new_doc['runs'] if r['device'] == new_label):
            title = f'{new_label}' + (f' (reference {base_label})' if base_label != new_label else '') + f' / {run["scene"]}'
            if run['scene'] not in bruns:
                print(f'\n## {title}: scene not in the reference run', file=out)
                continue
            b, n = bruns[run['scene']]['summary'], run['summary']
            per, overall = scene_verdicts(b, n)
            verdicts[(new_label, run['scene'])] = overall
            print(f'\n## {title}: {overall.upper()}  (windows {b.get("windows")} -> {n.get("windows")})', file=out)
            print(f'| {"metric":<16} | {"baseline":>9} | {"new":>9} | {"delta":>8} | verdict |', file=out)
            print(f'|{"-" * 18}|{"-" * 11}|{"-" * 11}|{"-" * 10}|---------|', file=out)
            for key, label, *_ in METRICS:
                delta, v = per[key]
                d = '-' if delta is None else (f'{delta:+.2f}' if isinstance(delta, float) else f'{delta:+d}')
                print(f'| {label:<16} | {fmt(b.get(key)):>9} | {fmt(n.get(key)):>9} | {d:>8} | {v:<7} |', file=out)
        for w in warns:
            print(f'WARNING {new_label}: not comparable: {w}', file=out)
    return verdicts
