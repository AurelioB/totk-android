#!/usr/bin/env python3
"""ledger.py: the append-only benchmark ledger, docs/benchmarks/ledger.jsonl (one JSON object per run and device), and
its human view docs/benchmarks/LEDGER.md.

  ledger.py add <result.json> --commit <sha> --apk <apk> [--run-id ID] [--profile TEXT] [--note TEXT] [--device LABEL ...]
      Record an analyze.py result. The commit must exist in this repository and be the commit the APK was built from
      (its assets/build.txt, not -dirty); the APK's sha256 must be the one collect.sh read from the device. Each device
      gets an entry and its own result file docs/benchmarks/runs/<run-id>/<device>.json, compared against the current
      baseline (docs/benchmarks/CURRENT_BASELINE) when the device is in it, and against the latest earlier entry for the
      same device model and settings_env. Re-renders LEDGER.md.
  ledger.py render                      rewrite LEDGER.md from ledger.jsonl
  ledger.py show <run-id>               print a run's entries
  ledger.py compare <run-id> <run-id>   compare two runs' stored results (as compare.py does)

Run ids are <date>-<label> (default: the result's date and name). Existing entries and result files are never changed."""
import argparse, copy, datetime, hashlib, json, os, re, subprocess, sys, zipfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from benchlib import (BENCH_DOCS, METRICS, REPO, comparability, compare_docs, current_baseline, load_result,  # noqa: E402
                      match_device, scene_verdicts)

LEDGER = BENCH_DOCS / 'ledger.jsonl'
LEDGER_MD = BENCH_DOCS / 'LEDGER.md'
RUNS = BENCH_DOCS / 'runs'


def rel(p):
    return str(Path(p).resolve().relative_to(REPO))


def entries():
    if not LEDGER.exists():
        return []
    return [json.loads(l) for l in LEDGER.read_text(encoding='utf-8').splitlines() if l.strip()]


def device_doc(doc, label):
    """The result document restricted to one device."""
    d = copy.deepcopy(doc)
    d['devices'] = {label: d['devices'][label]}
    d['runs'] = [r for r in d['runs'] if r['device'] == label]
    d['scenes'] = {s: v for s, v in d['scenes'].items() if any(r['scene'] == s for r in d['runs'])}
    return d


def verdict_block(ref_doc, ref_label, doc, label):
    refruns = {r['scene']: r for r in ref_doc['runs'] if r['device'] == ref_label}
    scenes = {}
    for r in doc['runs']:
        if r['device'] == label and r['scene'] in refruns:
            per, overall = scene_verdicts(refruns[r['scene']]['summary'], r['summary'])
            scenes[r['scene']] = {'overall': overall, 'metrics': {k: v for k, (_, v) in per.items()}}
    return {'device': ref_label, 'scenes': scenes, 'warnings': comparability(ref_doc, ref_label, doc, label)}


def git(*args):
    return subprocess.run(['git', '-C', str(REPO), *args], capture_output=True, text=True)


def cmd_add(a):
    doc = load_result(a.result)
    r = git('rev-parse', '--verify', f'{a.commit}^{{commit}}')
    if r.returncode:
        sys.exit(f'--commit {a.commit}: not a commit of this repository ({r.stderr.strip()})')
    commit = r.stdout.strip()
    try:
        with zipfile.ZipFile(a.apk) as z:
            build_txt = z.read('assets/build.txt').decode()
    except (OSError, KeyError, zipfile.BadZipFile) as e:
        sys.exit(f'--apk {a.apk}: cannot read assets/build.txt ({e})')
    m = re.fullmatch(r'commit ([0-9a-f]{7,40})(-dirty)?\n?', build_txt)
    if not m:
        sys.exit(f'--apk: unexpected assets/build.txt {build_txt!r}')
    if m[2]:
        sys.exit(f'--apk: built from a dirty tree ({build_txt.strip()}): rebuild from a clean commit to record it')
    if not commit.startswith(m[1]):
        sys.exit(f'--apk: built from {m[1]}, not from --commit {commit}')
    if doc['build'].get('commit') and not commit.startswith(str(doc['build']['commit']).removesuffix('-dirty')):
        sys.exit(f'result build.commit {doc["build"]["commit"]} is not --commit {commit}')
    apk_sha = hashlib.sha256(Path(a.apk).read_bytes()).hexdigest()
    measured = doc['build'].get('apk_sha256')
    if measured and measured != apk_sha:
        sys.exit(f'--apk sha256 {apk_sha} is not the measured (installed) APK {measured}')
    if not measured:
        print('ledger: warning: the result has no measured apk_sha256 (no facts from collect.sh); recording the given APK',
              file=sys.stderr)

    run_id = a.run_id or f'{doc["date"]}-{doc["name"]}'
    if not re.fullmatch(r'[A-Za-z0-9._-]+', run_id):
        sys.exit(f'run id {run_id!r}: letters, digits, . _ - only')
    labels = a.device or list(doc['devices'])
    old = entries()
    base_path = current_baseline()
    base_doc = load_result(base_path)
    new_entries = []
    for label in labels:
        if label not in doc['devices']:
            sys.exit(f'--device {label}: not in the result')
        if any(e['run_id'] == run_id and e['device'] == label for e in old):
            sys.exit(f'{run_id} / {label} is already in the ledger (append-only; use another --run-id)')
        out = RUNS / run_id / f'{label}.json'
        if out.exists():
            sys.exit(f'{rel(out)} exists; results are never overwritten')
        ddoc = device_doc(doc, label)
        dev = ddoc['devices'][label]
        facts = {k: dev.get(k) for k in ('name', 'soc', 'gpu', 'android', 'screens', 'serial_sha256')}
        facts['model'] = facts.pop('name')
        vs_base = None
        base_label = match_device(base_doc, label, dev)
        if base_label is not None:
            vs_base = {'against': rel(base_path), **verdict_block(base_doc, base_label, ddoc, label)}
        vs_prev = None
        for e in reversed(old + new_entries):
            if (e['device_facts'].get('model') or e['device']) == (facts['model'] or label) \
                    and e['settings_env'] == dev['settings_env']:
                prev_doc = load_result(REPO / e['result'])
                vs_prev = {'run_id': e['run_id'], **verdict_block(prev_doc, e['device'], ddoc, label)}
                break
        entry = {
            'run_id': run_id, 'device': label,
            'recorded_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds'),
            'date': doc['date'],
            'commit': commit, 'dirty': False, 'apk_sha256': apk_sha,
            'app_version': doc['build'].get('app_version'), 'version_code': doc['build'].get('version_code'),
            'game_version': doc['build'].get('game_version'),
            'turnip_driver_sha256': doc['build'].get('turnip_driver_sha256'),
            'baseline': rel(base_path),
            'device_facts': facts,
            'profile': a.profile or dev.get('profile') or 'default',
            'settings_env': dev['settings_env'],
            'scenes': [r['scene'] for r in ddoc['runs']],
            'summary': {r['scene']: r['summary'] for r in ddoc['runs']},
            'vs_baseline': vs_base,
            'vs_previous': vs_prev,
            'notes': a.note or '',
            'result': rel(out),
        }
        out.parent.mkdir(parents=True, exist_ok=True)
        with open(out, 'x', encoding='utf-8') as f:
            json.dump(ddoc, f, ensure_ascii=False, indent=2)
            f.write('\n')
        new_entries.append(entry)
    with open(LEDGER, 'a', encoding='utf-8') as f:
        for e in new_entries:
            f.write(json.dumps(e, ensure_ascii=False, sort_keys=False) + '\n')
    for e in new_entries:
        print(f'ledger: added {e["run_id"]} / {e["device"]} -> {e["result"]}')
        for kind in ('vs_baseline', 'vs_previous'):
            v = e[kind]
            if v:
                ref = v.get('against') or v.get('run_id')
                print(f'  {kind} ({ref}): ' + ', '.join(f'{s} {x["overall"]}' for s, x in v['scenes'].items()))
                for w in v['warnings']:
                    print(f'  WARNING not comparable: {w}')
            else:
                print(f'  {kind}: no matching reference')
    cmd_render(a)


def cmd_render(_a=None):
    rows = ['# Benchmark ledger', '',
            'Generated by `tools/bench/ledger.py render` from [`ledger.jsonl`](ledger.jsonl); do not edit by hand. '
            f'Current baseline: `{rel(current_baseline())}`. Verdicts follow `tools/bench/compare.py`; '
            '"!" marks a comparison with a settings.env or cpufreq-cap difference (not comparable).', '',
            '| Run | Device | Commit | App | Profile | Scene | Windows | fps mean | p50 ms | p95 ms | GPU ms | >50 ms | '
            'Max CPU / GPU °C | vs baseline | vs previous | Notes |',
            '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    f = lambda v: '-' if v is None else str(v)
    for e in entries():
        for scene in e['scenes']:
            s = e['summary'][scene]
            vv = []
            for kind in ('vs_baseline', 'vs_previous'):
                v = e.get(kind)
                if not v or scene not in v['scenes']:
                    vv.append('-')
                    continue
                ref = Path(v['against']).stem if kind == 'vs_baseline' else v['run_id']
                vv.append(f'{v["scenes"][scene]["overall"]}{" !" if v["warnings"] else ""} ({ref})')
            rows.append(f'| [{e["run_id"]}]({Path(e["result"]).relative_to("docs/benchmarks").as_posix()}) | '
                        f'{e["device_facts"].get("model") or e["device"]} | `{e["commit"][:12]}` | '
                        f'{f(e["app_version"])} ({f(e["version_code"])}) | {e["profile"]} | {scene} | {f(s["windows"])} | '
                        f'{f(s["fps_mean"])} | {f(s["p50_ms_median"])} | {f(s["p95_ms_max"])} | {f(s["gpu_ms_mean"])} | '
                        f'{f(s["over50_total"])} | {f(s["cpu_max_c"])} / {f(s["gpu_max_c"])} | {vv[0]} | {vv[1]} | '
                        f'{e["notes"].replace("|", "/")} |')
    LEDGER_MD.write_text('\n'.join(rows) + '\n', encoding='utf-8')
    print(f'ledger: wrote {rel(LEDGER_MD)} ({len(entries())} entries)')


def run_doc(run_id):
    es = [e for e in entries() if e['run_id'] == run_id]
    if not es:
        sys.exit(f'no ledger run {run_id}')
    doc = None
    for e in es:
        d = load_result(REPO / e['result'])
        if doc is None:
            doc = d
        else:
            doc['devices'].update(d['devices'])
            doc['runs'] += d['runs']
            doc['scenes'].update(d['scenes'])
    doc['name'] = run_id
    return es, doc


def cmd_show(a):
    es, _ = run_doc(a.run_id)
    for e in es:
        print(f'## {e["run_id"]} / {e["device"]}: {e["device_facts"].get("model")} ({e["device_facts"].get("soc")}, '
              f'{e["device_facts"].get("gpu")}, Android {e["device_facts"].get("android")})')
        print(f'commit {e["commit"]}  app {e["app_version"]} ({e["version_code"]})  apk {e["apk_sha256"]}')
        print(f'profile {e["profile"]}  settings_env {" ".join(f"{k}={v}" for k, v in e["settings_env"].items())}')
        print(f'result {e["result"]}  baseline {e["baseline"]}' + (f'\nnotes: {e["notes"]}' if e['notes'] else ''))
        for scene in e['scenes']:
            s = e['summary'][scene]
            print(f'  {scene}: ' + ', '.join(f'{label} {s.get(k)}' for k, label, *_ in METRICS))
            for kind in ('vs_baseline', 'vs_previous'):
                v = e.get(kind)
                if v and scene in v['scenes']:
                    print(f'    {kind} ({v.get("against") or v.get("run_id")}): {v["scenes"][scene]["overall"]}')
        for kind in ('vs_baseline', 'vs_previous'):
            for w in (e.get(kind) or {}).get('warnings', []):
                print(f'  WARNING {kind} not comparable: {w}')


def cmd_compare(a):
    _, da = run_doc(a.a)
    _, db = run_doc(a.b)
    print(f'# {a.b} vs {a.a}')
    compare_docs(da, db)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], epilog=__doc__.split('\n\n', 1)[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('add')
    p.add_argument('result')
    p.add_argument('--commit', required=True)
    p.add_argument('--apk', required=True)
    p.add_argument('--run-id')
    p.add_argument('--profile', help='profile description (default: the result\'s, else "default")')
    p.add_argument('--note')
    p.add_argument('--device', action='append', help='only these device labels (default: all in the result)')
    p.set_defaults(fn=cmd_add)
    sub.add_parser('render').set_defaults(fn=cmd_render)
    p = sub.add_parser('show')
    p.add_argument('run_id')
    p.set_defaults(fn=cmd_show)
    p = sub.add_parser('compare')
    p.add_argument('a', metavar='run-id-a', help='reference run')
    p.add_argument('b', metavar='run-id-b', help='new run')
    p.set_defaults(fn=cmd_compare)
    a = ap.parse_args()
    a.fn(a)


if __name__ == '__main__':
    main()
