#!/usr/bin/env python3
"""ledger.py: the append-only benchmark ledger, docs/benchmarks/ledger.jsonl (one JSON object per run and device), and
its human view docs/benchmarks/LEDGER.md.

  ledger.py add <result.json> --commit <sha> --apk <apk> --profile TEXT [--run-id ID] [--note TEXT] [--device LABEL ...]
      Record an analyze.py result. The commit must exist in this repository and be the commit the APK was built from
      (its assets/build.txt, not -dirty); the APK's sha256 must be the one collect.sh read from the device. Each device
      gets an entry and its own result file docs/benchmarks/runs/<run-id>/<device>.json.
      Baselines are per frequency signature (benchlib.signature_of: model + SoC, settings_env hash, per-cluster CPU caps,
      GPU cap). A device whose signature has a baseline (docs/benchmarks/baselines/index.json) is compared against it.
      A device whose signature has none, with every run freq_stable, BECOMES the baseline of that signature: its result
      is written as the new immutable docs/benchmarks/baselines/<device-slug>/<signature-short>.json, pinned in
      BASELINES.sha256 and added to the index (the only way a baseline is created). Runs whose frequency limits changed
      (freq_stable false) or were not recorded are recorded but neither become nor are compared as a baseline.
      Also compared, for information, against a reference-only baseline (no GPU cap recorded: baseline-1.0.0.json)
      with the same model, settings and CPU caps, and against the latest earlier entry with the same signature.
      Re-renders LEDGER.md.
  ledger.py render                      rewrite LEDGER.md from ledger.jsonl
  ledger.py show <run-id>               print a run's entries (build, profile, settings, frequency caps, method, scenes)
  ledger.py compare <run-id> <run-id>   compare two runs' stored results (as compare.py does)

Run ids are <date>-<label> (default: the result's date and name). Existing entries and result files are never changed."""
import argparse, copy, datetime, hashlib, json, os, re, subprocess, sys, zipfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from benchlib import (BENCH_DOCS, METRICS, REPO, comparability, compare_docs, device_signature, index_entry,  # noqa: E402
                      index_sig, load_index, load_result, scene_verdicts, sig_caps_text, signature_relation,
                      write_baseline)

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


def baseline_block(b, ddoc, label, relation):
    return {'against': rel(BENCH_DOCS / b['file']), 'relation': relation, 'signature': b['signature'],
            **verdict_block(load_result(BENCH_DOCS / b['file']), b['device'], ddoc, label)}


def compact_limits(fl):
    """The freq_limits snapshot without the GPU frequency table (kept in the result file)."""
    if not fl:
        return None
    return {'cpu': fl['cpu'], 'gpu': {k: v for k, v in (fl.get('gpu') or {}).items() if k != 'available_hz'}}


def resolve_baseline(ddoc, label, run_id, commit, docs=None):
    """(signature, freq_stable, baseline index entry or None, became_baseline) of one device's result: the existing
    baseline of its frequency signature, else (all runs freq_stable) the result itself becomes it (write_baseline)."""
    sig = device_signature(ddoc, label)
    stables = [r.get('freq_stable') for r in ddoc['runs']]
    freq_stable = False if False in stables else True if stables and all(s is True for s in stables) else None
    base = index_entry(sig['sha256'], docs) if sig else None
    if sig and freq_stable and not base:
        return sig, freq_stable, write_baseline(ddoc, label, run_id, commit, docs), True
    return sig, freq_stable, base, False


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
    for label in labels:
        if label not in doc['devices']:
            sys.exit(f'--device {label}: not in the result')
        if any(e['run_id'] == run_id and e['device'] == label for e in old):
            sys.exit(f'{run_id} / {label} is already in the ledger (append-only; use another --run-id)')
        if (RUNS / run_id / f'{label}.json').exists():
            sys.exit(f'{rel(RUNS / run_id / f"{label}.json")} exists; results are never overwritten')
    new_entries = []
    for label in labels:
        out = RUNS / run_id / f'{label}.json'
        ddoc = device_doc(doc, label)
        dev = ddoc['devices'][label]
        facts = {k: dev.get(k) for k in ('name', 'soc', 'gpu', 'android', 'screens', 'serial_sha256')}
        facts['model'] = facts.pop('name')
        sig, freq_stable, base, became = resolve_baseline(ddoc, label, run_id, commit)
        vs_base = baseline_block(base, ddoc, label, 'match') if base and freq_stable and not became else None
        vs_ref = None
        if sig and freq_stable:
            for b in load_index()['baselines']:
                if signature_relation(index_sig(b), sig) == 'reference-only':
                    vs_ref = baseline_block(b, ddoc, label, 'reference-only')
                    break
        vs_prev = None
        if sig and freq_stable:
            for e in reversed(old + new_entries):
                if e.get('freq_signature') == sig['sha256'] and e.get('freq_stable'):
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
            'device_facts': facts,
            'profile': a.profile,
            'settings_env': dev['settings_env'],
            'freq_signature': sig['sha256'] if sig else None,
            'freq_caps': sig_caps_text(sig) if sig else None,
            'freq_limits': compact_limits(dev.get('freq_limits')),
            'freq_stable': freq_stable,
            'baseline': rel(BENCH_DOCS / base['file']) if base else None,
            'became_baseline': became,
            'scenes': [r['scene'] for r in ddoc['runs']],
            'summary': {r['scene']: r['summary'] for r in ddoc['runs']},
            'vs_baseline': vs_base,
            'vs_reference': vs_ref,
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
        print(f'  frequency signature {(e["freq_signature"] or "none")[:12]}: {e["freq_caps"] or "no frequency limits recorded"}'
              + {True: '', False: ' (CHANGED during a run: freq_stable false)', None: ' (stability unknown)'}[e['freq_stable']])
        if e['became_baseline']:
            print(f'  NEW BASELINE for this signature: {e["baseline"]} (pinned, docs/benchmarks/baselines/index.json)')
        elif not e['freq_stable']:
            print('  not used as or compared with a baseline: the frequency limits were not stable / not recorded')
        for kind in ('vs_baseline', 'vs_reference', 'vs_previous'):
            v = e[kind]
            if v:
                ref = v.get('against') or v.get('run_id')
                tag = ' (REFERENCE ONLY: no GPU cap recorded there)' if kind == 'vs_reference' else ''
                print(f'  {kind} ({ref}){tag}: ' + ', '.join(f'{s} {x["overall"]}' for s, x in v['scenes'].items()))
                for w in v['warnings']:
                    print(f'  WARNING not comparable: {w}')
            elif not (kind == 'vs_baseline' and e['became_baseline']):
                print(f'  {kind}: no matching reference')
    cmd_render(a)


def entry_caps(e, index):
    """(caps text, signature short, stable) of an entry; 1.0.0 entries take theirs from the baseline index."""
    if e.get('freq_signature'):
        return e['freq_caps'], e['freq_signature'][:12], e.get('freq_stable')
    b = next((b for b in index if b['run_id'] == e['run_id'] and b['device'] == e['device']), None)
    return (b['caps'], b['short'], True) if b else (None, None, None)


def cmd_render(_a=None):
    index = load_index()['baselines']
    rows = ['# Benchmark ledger', '',
            'Generated by `tools/bench/ledger.py render` from [`ledger.jsonl`](ledger.jsonl); do not edit by hand. '
            'Baselines are per frequency signature (device model + SoC, settings.env, CPU caps per cluster, GPU cap): '
            '[`baselines/index.json`](baselines/index.json). Caps are the effective limits during the run (cpufreq '
            'scaling_max_freq per cluster in GHz, GPU cap in MHz); "(max)" means at the hardware maximum, "(hw ...)" '
            'gives the hardware maximum when capped below it. Verdicts follow `tools/bench/compare.py`; "!" marks a '
            'comparison with a warning (not comparable); "reference only" compares against a baseline without a '
            'recorded GPU cap.', '',
            '| Run | Device | Commit | App | Profile | Frequency caps (signature) | Scene | Windows | fps mean | p50 ms | '
            'p95 ms | GPU ms | >50 ms | Max CPU / GPU °C | vs baseline | vs previous | Notes |',
            '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    f = lambda v: '-' if v is None else str(v)
    for e in entries():
        caps, short, stable = entry_caps(e, index)
        caps_cell = '-' if not caps else f'{caps} (`{short}`)' + (' **CHANGED during the run**' if stable is False else '')
        for scene in e['scenes']:
            s = e['summary'][scene]
            vv = []
            for kind in ('vs_baseline', 'vs_previous'):
                v = e.get(kind)
                r = e.get('vs_reference') or {}
                ref_only = (f'{r["scenes"][scene]["overall"]}{" !" if r["warnings"] else ""} '
                            f'(reference only: {Path(r["against"]).stem})') if scene in r.get('scenes', {}) else None
                if kind == 'vs_baseline' and e.get('became_baseline'):
                    vv.append('new baseline' + (f'; {ref_only}' if ref_only else ''))
                    continue
                if kind == 'vs_baseline' and not v and ref_only:
                    vv.append(ref_only)
                    continue
                if not v or scene not in v['scenes']:
                    vv.append('-')
                    continue
                ref = Path(v['against']).stem if kind == 'vs_baseline' else v['run_id']
                vv.append(f'{v["scenes"][scene]["overall"]}{" !" if v["warnings"] else ""} ({ref})')
            rows.append(f'| [{e["run_id"]}]({Path(e["result"]).relative_to("docs/benchmarks").as_posix()}) | '
                        f'{e["device_facts"].get("model") or e["device"]} | `{e["commit"][:12]}` | '
                        f'{f(e["app_version"])} ({f(e["version_code"])}) | {e["profile"]} | {caps_cell} | {scene} | '
                        f'{f(s["windows"])} | {f(s["fps_mean"])} | {f(s["p50_ms_median"])} | {f(s["p95_ms_max"])} | '
                        f'{f(s["gpu_ms_mean"])} | {f(s["over50_total"])} | {f(s["cpu_max_c"])} / {f(s["gpu_max_c"])} | '
                        f'{vv[0]} | {vv[1]} | {e["notes"].replace("|", "/")} |')
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


def registered_file(run_id):
    """The baseline file registered in the index for a ledger run without recorded frequency limits (1.0.0)."""
    b = next((b for b in load_index()['baselines'] if b['run_id'] == run_id), None)
    return BENCH_DOCS / b['file'] if b else None


def cmd_show(a):
    es, _ = run_doc(a.run_id)
    index = load_index()['baselines']
    for e in es:
        print(f'## {e["run_id"]} / {e["device"]}: {e["device_facts"].get("model")} ({e["device_facts"].get("soc")}, '
              f'{e["device_facts"].get("gpu")}, Android {e["device_facts"].get("android")})')
        print(f'commit {e["commit"]}  app {e["app_version"]} ({e["version_code"]})  apk {e["apk_sha256"]}')
        print(f'game {e["game_version"]}  turnip driver {e["turnip_driver_sha256"]}')
        print(f'profile {e["profile"]}  settings_env {" ".join(f"{k}={v}" for k, v in e["settings_env"].items())}')
        caps, short, stable = entry_caps(e, index)
        print(f'frequency caps {caps or "not recorded"}' + (f'  signature {short}' if short else '')
              + {True: '', False: '  CHANGED during a run (freq_stable false)', None: ''}[stable])
        for c in (e.get('freq_limits') or {}).get('cpu', []):
            print(f'  cpufreq policy{c["policy"]} cpus {",".join(map(str, c["related_cpus"]))}: scaling '
                  f'{c["scaling_min_khz"]}-{c["scaling_max_khz"]} kHz, hardware max {c["cpuinfo_max_khz"]} kHz')
        g = (e.get('freq_limits') or {}).get('gpu')
        if g:
            print(f'  GPU cap {g.get("cap_hz")} Hz (max_gpuclk {g.get("max_gpuclk_hz")}, devfreq max '
                  f'{g.get("devfreq_max_hz")}), hardware max {g.get("hw_max_hz")} Hz, power levels min '
                  f'{g.get("min_pwrlevel")} max {g.get("max_pwrlevel")} thermal {g.get("thermal_pwrlevel")}')
        print(f'result {e["result"]}  baseline {e["baseline"]}' + ('  (became the baseline)' if e.get('became_baseline') else '')
              + (f'\nnotes: {e["notes"]}' if e['notes'] else ''))
        d = load_result(REPO / e['result'])
        print(f'method {d["schema"]}: {d["method"]["summary"]}')
        for scene in e['scenes']:
            sc = d['scenes'].get(scene, {})
            pans = [p for r in d['runs'] if r['scene'] == scene for p in r.get('pans', [])]
            secs = [round((datetime.datetime.fromisoformat(p['end_utc']) -
                           datetime.datetime.fromisoformat(p['start_utc'])).total_seconds()) for p in pans]
            print(f'  scene {scene}: {sc.get("name")}, save {sc.get("save_slot")}; pans '
                  + ', '.join(f'{s} s at stick {p["stick_value"]}' for s, p in zip(secs, pans)))
            s = e['summary'][scene]
            print('    ' + ', '.join(f'{label} {s.get(k)}' for k, label, *_ in METRICS))
            for kind in ('vs_baseline', 'vs_reference', 'vs_previous'):
                v = e.get(kind)
                if v and scene in v['scenes']:
                    tag = ' (reference only)' if kind == 'vs_reference' else ''
                    print(f'    {kind} ({v.get("against") or v.get("run_id")}){tag}: {v["scenes"][scene]["overall"]}')
        for kind in ('vs_baseline', 'vs_reference', 'vs_previous'):
            for w in (e.get(kind) or {}).get('warnings', []):
                print(f'  WARNING {kind} not comparable: {w}')


def cmd_compare(a):
    _, da = run_doc(a.a)
    _, db = run_doc(a.b)
    print(f'# {a.b} vs {a.a}')
    compare_docs(da, db, base_path=registered_file(a.a), new_path=registered_file(a.b))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], epilog=__doc__.split('\n\n', 1)[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('add')
    p.add_argument('result')
    p.add_argument('--commit', required=True)
    p.add_argument('--apk', required=True)
    p.add_argument('--run-id')
    p.add_argument('--profile', required=True,
                   help='the profile measured: the Graphics preset marked IN USE, e.g. "Balanced (default: Recommended '
                        'for this device)", or a description of a deliberate non-default profile')
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
