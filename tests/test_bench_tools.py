"""The benchmark tools (tools/bench): the immutable baselines match their pins, the baseline write guard, frequency
limits and signatures, automatic per-signature baselines, the comparison verdicts and signature checks, and the ledger
stays consistent with its stored results; no device needed."""
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BENCH = ROOT / 'tools/bench'
sys.path[:0] = [str(BENCH)]
import analyze  # noqa: E402
import benchlib  # noqa: E402
import check_baselines  # noqa: E402
import ledger  # noqa: E402

BASELINE = benchlib.BENCH_DOCS / 'baseline-1.0.0.json'
# A Thor snapshot as `bench_input.sh freqs` prints it (no cap below the hardware maximum).
THOR_FREQS = ('cpu0=0,1,2:902400:2016000:2016000 cpu3=3,4,5,6:1651200:2803200:2803200 cpu7=7:1843200:3187200:3187200 '
              'gpu_maxclk=680000000 gpu_freqs=680000000,615000000,550000000,475000000,401000000,348000000,295000000,'
              '220000000,124800000 gpu_devfreq_max=680000000 gpu_min_pwrlevel=4 gpu_max_pwrlevel=0 gpu_thermal_pwrlevel=0')


def run(*args):
    return subprocess.run([sys.executable, *map(str, args)], capture_output=True, text=True, cwd=ROOT)


def snapshot(text=THOR_FREQS):
    return benchlib.parse_freq_tokens(text.split())


def thor_result(fl=None, stable=True):
    """The 1.0.0 Thor result restricted to Thor, with recorded frequency limits (a new-style result)."""
    doc = ledger.device_doc(benchlib.load_result(BASELINE), 'thor')
    doc['devices']['thor']['freq_limits'] = fl or snapshot()
    for r in doc['runs']:
        r['freq_stable'] = stable
    return doc


class Baselines(unittest.TestCase):
    def test_baselines_match_their_pins(self):
        self.assertEqual(check_baselines.problems(), [])

    def test_analyze_and_compare_refuse_to_write_a_baseline(self):
        for target in (BASELINE, benchlib.BENCH_DOCS / 'baselines/test/x.json'):
            r = run(BENCH / 'analyze.py', ROOT / 'docs', '--json', target)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn('immutable', r.stderr)
        r = run(BENCH / 'compare.py', BASELINE, BASELINE, '--out', ROOT / 'docs/benchmarks/baseline-test.md')
        self.assertNotEqual(r.returncode, 0)
        self.assertFalse((ROOT / 'docs/benchmarks/baseline-test.md').exists())

    def test_index_registers_the_1_0_0_runs_without_a_gpu_cap(self):
        idx = {b['device']: b for b in benchlib.load_index()['baselines'] if b['file'] == 'baseline-1.0.0.json'}
        self.assertEqual(set(idx), {'thor', 'odin', 'duo'})
        self.assertTrue(all(b['gpu_cap_hz'] is None for b in idx.values()))
        self.assertEqual(idx['odin']['cpu_caps_khz'], [2227200, 2246400])
        self.assertEqual(idx['odin']['cpu_hw_max_khz'], [3532800, 4320000])
        self.assertEqual(idx['thor']['cpu_caps_khz'], idx['thor']['cpu_hw_max_khz'])


class Signatures(unittest.TestCase):
    def test_parse_snapshot(self):
        fl = snapshot()
        self.assertEqual([c['related_cpus'] for c in fl['cpu']], [[0, 1, 2], [3, 4, 5, 6], [7]])
        self.assertEqual(fl['cpu'][0]['scaling_min_khz'], 902400)
        self.assertEqual(benchlib.caps_of_limits(fl), ((2016000, 2803200, 3187200), 680000000))
        self.assertEqual(fl['gpu']['hw_max_hz'], 680000000)
        # kgsl max_pwrlevel 2 caps the GPU at the third table entry, below max_gpuclk
        capped = snapshot(THOR_FREQS.replace('gpu_max_pwrlevel=0', 'gpu_max_pwrlevel=2'))
        self.assertEqual(capped['gpu']['cap_hz'], 550000000)

    def test_signature_fields(self):
        env = {'TOTK_FPS60': '0', 'TOTK_DATA_DIR': '/x'}
        base, fields = benchlib.signature_of('AYN Thor', 'QTI QCS8550', env, [2016000, 2803200], 680000000)
        self.assertEqual(base, benchlib.signature_of('AYN Thor', 'QTI QCS8550', dict(env, TOTK_DATA_DIR='/y'),
                                                     [2016000, 2803200], 680000000)[0])   # the data dir is ignored
        self.assertEqual(base, benchlib.signature_of_fields(fields))
        for changed in (('AYN Odin 3', 'QTI QCS8550', env, [2016000, 2803200], 680000000),
                        ('AYN Thor', 'QTI QCS8550', dict(env, TOTK_FPS60='1'), [2016000, 2803200], 680000000),
                        ('AYN Thor', 'QTI QCS8550', env, [1804800, 2803200], 680000000),
                        ('AYN Thor', 'QTI QCS8550', env, [2016000, 2803200], 615000000),
                        ('AYN Thor', 'QTI QCS8550', env, [2016000, 2803200], None)):
            self.assertNotEqual(base, benchlib.signature_of(*changed)[0], changed)

    def test_caps_text_marks_hardware_maximum(self):
        self.assertEqual(benchlib.caps_text([2016000, 2803200], [2016000, 2803200], 680000000, 680000000),
                         'CPU 2.02/2.80 GHz (max), GPU 680 MHz (max)')
        self.assertEqual(benchlib.caps_text([2227200, 2246400], [3532800, 4320000], None, None),
                         'CPU 2.23/2.25 GHz (hw 3.53/4.32), GPU cap unknown')


class Compare(unittest.TestCase):
    def setUp(self):
        self.doc = benchlib.load_result(BASELINE)

    def test_baseline_against_itself_is_all_same(self):
        r = run(BENCH / 'compare.py', BASELINE, BASELINE)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('frequency signatures match', r.stdout)
        self.assertNotIn('better', r.stdout.replace('0 better', ''))
        self.assertNotIn('worse', r.stdout.replace('0 worse', ''))
        self.assertNotIn('WARNING', r.stdout)

    def test_thresholds(self):
        base = {'windows': 4, 'fps_mean': 30.0, 'p50_ms_median': 33.3, 'p95_ms_max': 38.0, 'gpu_ms_mean': 20.0,
                'over50_total': 4}
        self.assertEqual(benchlib.scene_verdicts(base, dict(base, fps_mean=30.5))[1], 'same')
        self.assertEqual(benchlib.scene_verdicts(base, dict(base, fps_mean=28.0))[1], 'worse')
        self.assertEqual(benchlib.scene_verdicts(base, dict(base, gpu_ms_mean=18.0))[1], 'better')
        # >50 ms frames compare per window: 8 over 8 windows is the same rate as 4 over 4
        self.assertEqual(benchlib.scene_verdicts(base, dict(base, windows=8, over50_total=8))[1], 'same')
        self.assertEqual(benchlib.scene_verdicts(base, dict(base, over50_total=12))[1], 'worse')

    def test_settings_and_cpu_cap_differences_are_flagged(self):
        new = json.loads(json.dumps(self.doc))
        label = next(iter(new['devices']))
        new['devices'][label]['settings_env']['TOTK_FPS60'] = 'changed'
        for r in new['runs']:
            if r['device'] == label:
                for t in r['thermal']:
                    t['cpufreq_caps_khz'] = [{'scaling_max': 1, 'cpuinfo_max': 2}]
                r['freq_stable'] = False
        warns = benchlib.comparability(self.doc, label, new, label)
        self.assertTrue(any('settings_env' in w for w in warns))
        self.assertTrue(any('cpufreq caps' in w for w in warns))
        self.assertTrue(any('freq_stable false' in w for w in warns))

    def compare(self, new, base=None, base_path=BASELINE):
        buf = io.StringIO()
        verdicts = benchlib.compare_docs(base or self.doc, new, buf, base_path=base_path)
        return {rel for _, rel in verdicts.values()}, buf.getvalue()

    def test_reference_only_against_1_0_0_when_cpu_caps_match(self):
        rels, text = self.compare(thor_result())
        self.assertEqual(rels, {'reference-only'})
        self.assertIn('REFERENCE ONLY', text)

    def test_signature_mismatch_is_not_comparable(self):
        capped = snapshot(THOR_FREQS.replace(':2016000:2016000', ':1804800:2016000'))
        rels, text = self.compare(thor_result(capped))
        self.assertEqual(rels, {'mismatch'})
        self.assertIn('NOT COMPARABLE', text)
        # the same mismatch between two new-style results (different GPU caps)
        gpu = snapshot(THOR_FREQS.replace('gpu_maxclk=680000000', 'gpu_maxclk=615000000'))
        rels, text = self.compare(thor_result(gpu), base=thor_result(), base_path=None)
        self.assertEqual(rels, {'mismatch'})
        self.assertIn('gpu_cap_hz: 680000000 -> 615000000', text)
        rels, _ = self.compare(thor_result(), base=thor_result(), base_path=None)
        self.assertEqual(rels, {'match'})


class AutoBaseline(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.docs = Path(self.tmp.name)
        (self.docs / 'BASELINES.sha256').write_text('')

    def tearDown(self):
        self.tmp.cleanup()

    def test_new_signature_becomes_its_baseline_once(self):
        doc = thor_result()
        sig, stable, base, became = ledger.resolve_baseline(doc, 'thor', 'run-a', 'c' * 40, self.docs)
        self.assertTrue(stable and became)
        self.assertEqual(base['signature'], sig['sha256'])
        path = self.docs / base['file']
        self.assertEqual(base['file'], f'baselines/ayn-thor/{sig["short"]}.json')
        self.assertTrue(path.exists())
        self.assertIn(base['file'], benchlib.read_pins(self.docs))
        self.assertEqual(check_baselines.problems(self.docs), [])
        self.assertEqual(json.loads(path.read_text())['baseline']['run_id'], 'run-a')
        before = path.read_bytes()
        # a later run with the same signature is compared against it, never replaces it
        _, _, base2, became2 = ledger.resolve_baseline(thor_result(), 'thor', 'run-b', 'd' * 40, self.docs)
        self.assertFalse(became2)
        self.assertEqual(base2['file'], base['file'])
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(len(benchlib.load_index(self.docs)['baselines']), 1)
        # another GPU cap is a new signature with its own baseline
        gpu = snapshot(THOR_FREQS.replace('gpu_maxclk=680000000', 'gpu_maxclk=615000000'))
        _, _, base3, became3 = ledger.resolve_baseline(thor_result(gpu), 'thor', 'run-c', 'e' * 40, self.docs)
        self.assertTrue(became3)
        self.assertNotEqual(base3['file'], base['file'])
        self.assertEqual(check_baselines.problems(self.docs), [])

    def test_refuses_to_overwrite_or_replace(self):
        doc = thor_result()
        _, _, base, _ = ledger.resolve_baseline(doc, 'thor', 'run-a', 'c' * 40, self.docs)
        with self.assertRaises(SystemExit):
            benchlib.write_baseline(doc, 'thor', 'run-b', 'd' * 40, self.docs)   # signature already has a baseline
        idx = benchlib.load_index(self.docs)
        idx['baselines'] = []
        benchlib.save_index(idx, self.docs)
        with self.assertRaises(SystemExit):
            benchlib.write_baseline(doc, 'thor', 'run-b', 'd' * 40, self.docs)   # the file exists / is pinned
        with self.assertRaises(SystemExit):
            benchlib.pin(base['file'], self.docs)
        self.assertTrue(benchlib.is_baseline_path(self.docs / base['file'], self.docs))
        (self.docs / base['file']).write_text('{}')
        self.assertTrue(any('CHANGED' in p for p in check_baselines.problems(self.docs)))

    def test_unstable_or_unrecorded_runs_never_become_baselines(self):
        _, stable, base, became = ledger.resolve_baseline(thor_result(stable=False), 'thor', 'r', 'c' * 40, self.docs)
        self.assertEqual((stable, base, became), (False, None, False))
        doc = thor_result()
        del doc['devices']['thor']['freq_limits']
        sig, _, base, became = ledger.resolve_baseline(doc, 'thor', 'r', 'c' * 40, self.docs)
        self.assertEqual((sig, base, became), (None, None, False))
        self.assertFalse((self.docs / 'baselines').exists())


class FreqStability(unittest.TestCase):
    def rows(self, *gcaps, caps=((2016000, 2016000), (2803200, 2803200), (3187200, 3187200))):
        return [{'caps': list(caps), 'gcap': g} for g in gcaps]

    def test_stable_run(self):
        s = snapshot()
        self.assertEqual(analyze.freq_stability([s, s], self.rows(680000000, 680000000)), (True, []))
        self.assertEqual(analyze.freq_stability([], []), (None, []))

    def test_cap_change_during_a_run(self):
        s = snapshot()
        lowered = snapshot(THOR_FREQS.replace(':3187200:3187200', ':2496000:3187200'))
        stable, changes = analyze.freq_stability([s, lowered], self.rows(680000000))
        self.assertFalse(stable)
        self.assertTrue(any('start / end' in c for c in changes))
        stable, changes = analyze.freq_stability([s, s], self.rows(680000000, 550000000))
        self.assertFalse(stable)
        self.assertTrue(any('max_gpuclk' in c for c in changes))
        rows = self.rows(680000000, caps=((2016000, 2016000), (2803200, 2803200), (2496000, 3187200)))
        self.assertFalse(analyze.freq_stability([s, s], self.rows(680000000) + rows)[0])
        # caps different from the device's first scene
        self.assertFalse(analyze.freq_stability([lowered, lowered], [], benchlib.caps_of_limits(s))[0])

    def test_parse_freqs_file(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'freqs.txt'
            p.write_text(f'thor rotsumamu T0=100 start {THOR_FREQS}\nthor rotsumamu T0=100 end \n')
            got = analyze.parse_freqs(p)
        self.assertEqual(list(got), [('thor', 100)])
        self.assertEqual(set(got[('thor', 100)]), {'start'})


class Ledger(unittest.TestCase):
    def test_entries_point_at_existing_results_of_clean_commits(self):
        path = benchlib.BENCH_DOCS / 'ledger.jsonl'
        seen = set()
        for line in path.read_text(encoding='utf-8').splitlines():
            e = json.loads(line)
            key = (e['run_id'], e['device'])
            self.assertNotIn(key, seen)
            seen.add(key)
            self.assertFalse(e['dirty'])
            self.assertRegex(e['commit'], r'^[0-9a-f]{40}$')
            result = benchlib.load_result(ROOT / e['result'])
            self.assertEqual(list(result['devices']), [e['device']])
            self.assertEqual({r['scene']: r['summary'] for r in result['runs']}, e['summary'])
            if e.get('freq_signature'):
                sig = benchlib.device_signature(result, e['device'])
                self.assertEqual(sig['sha256'], e['freq_signature'])
                if e.get('became_baseline'):
                    b = benchlib.index_entry(e['freq_signature'])
                    self.assertEqual(ROOT / e['baseline'], benchlib.BENCH_DOCS / b['file'])


def ab_log(windows, ctl):
    """A synthetic totk.log: per 300-frame window end f, its timing / fps / GPU / stutter lines (values from
    windows[f] = (fps, p95, gpu, over50)), with the [totk-ctl] lines (frame, text) placed where they happened."""
    lines, events = [], sorted(ctl)
    for f in range(300, max(windows) + 1, 300):
        while events and events[0][0] < f:
            lines.append(events.pop(0)[1])
        fps, p95, gpu, over50 = windows.get(f, (30.0, 40.0, 20.0, 0))
        lines += [f'[nvn] 计时 至帧 {f}： 帧时间 p50 33.30 / p95 {p95:.2f} / 最大 60.00 ms（300 帧）；',
                  f'[nvn-vk] frame {f}: {fps:.1f} fps over the last 300, 1000 draws',
                  f'[nvn-vk] frame {f}, per frame: recording 2.0 ms, waiting for the GPU 1.0 ms, GPU {gpu:.1f} ms',
                  f'[nvn-vk] frame {f}, stutter: longest frame 60.0 ms, 3 frames over 33 ms, {over50} over 50 ms']
    return '\n'.join(lines + [e[1] for e in events]) + '\n'


class InSessionAB(unittest.TestCase):
    CTL = [(1000, '[totk-ctl] gen=1 frame=1000 applied unset TOTK_VK_DESC_STOCK'),
           (1000, '[totk-ctl] gen=1 frame=1000 mark ab arm=A block=1 scene=hateno'),
           (2500, '[totk-ctl] gen=2 frame=2500 applied TOTK_VK_DESC_STOCK=1'),
           (2500, '[totk-ctl] gen=2 frame=2500 mark ab arm=B block=2 scene=hateno'),
           (4000, '[totk-ctl] gen=3 frame=4000 mark ab arm=B block=3 scene=hateno'),
           (5200, '[totk-ctl] gen=4 frame=5200 applied unset TOTK_VK_DESC_STOCK'),
           (5200, '[totk-ctl] gen=4 frame=5200 mark ab arm=A block=4 scene=hateno'),
           (6500, '[totk-ctl] gen=5 frame=6500 mark ab done scene=hateno')]

    def test_parse_ctl(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'x.log'
            p.write_text('\n'.join(t for _, t in self.CTL) + '\n[totk-ctl] refused "x": unknown command\n', encoding='utf-8')
            gens = analyze.parse_ctl(p)
        self.assertEqual([(g['gen'], g['frame'], g['arm'], g['block']) for g in gens],
                         [(1, 1000, 'A', 1), (2, 2500, 'B', 2), (3, 4000, 'B', 3), (4, 5200, 'A', 4), (5, 6500, None, None)])
        self.assertEqual(gens[1]['applied'], ['TOTK_VK_DESC_STOCK=1'])

    def test_window_generation(self):
        gens = [{'gen': 1, 'frame': 1000, 'arm': 'A'}, {'gen': 2, 'frame': 2500, 'arm': 'B'}]
        wg = lambda s, e, guard=2: (lambda g: g and g['gen'])(analyze.window_generation(s, e, gens, guard))
        self.assertEqual(wg(1, 300), 0)            # before any live change
        self.assertEqual(wg(701, 1000), 0)         # the change applies after frame 1000: the window ends before it
        self.assertEqual(wg(901, 1200), None)      # straddles frame 1000
        self.assertEqual(wg(1003, 1302), 1)        # starts after the change plus the guard
        self.assertEqual(wg(1002, 1301), None)     # inside the guard
        self.assertEqual(wg(1002, 1301, guard=0), 1)
        self.assertEqual(wg(2201, 2500), 1)        # ends on the frame the next change follows
        self.assertEqual(wg(2202, 2501), None)
        self.assertEqual(wg(2801, 3100), 2)

    def make_run(self, d, a_fps=(30.0, 30.2), b_fps=(28.0, 28.1)):
        """Four ab.sh blocks A B B A, one pan each: block 3's last window straddles the change to block 4."""
        w = {}
        for f in (1800, 2100, 2400):
            w[f] = (a_fps[0], 40.0, 20.0, 1)
        for f in (3000, 3300, 3600, 3900):
            w[f] = (b_fps[0], 42.0, 20.0, 2)
        for f in (4500, 4800, 5100, 5400):
            w[f] = (b_fps[1], 43.0, 20.1, 2)
        w[6000] = (a_fps[1], 40.5, 19.9, 1)   # the last pan holds one whole window
        (d / 'logs').mkdir()
        (d / 'logs/thor.log').write_text(ab_log(w, self.CTL), encoding='utf-8')
        (d / 'pans.txt').write_text(''.join(f'thor hateno F0={f0} F1={f1} T0={t} T1={t + 30} V=16000 D={dd}\n' for f0, f1, t, dd in
                                            ((1200, 2400, 100, 1), (2400, 3900, 200, -1), (3900, 5400, 300, 1), (5400, 6000, 400, -1))))

    def test_analyze_arms_and_abtest(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            self.make_run(d)
            r = run(BENCH / 'analyze.py', d, '--arms', '--json', d / 'ab.json')
            self.assertEqual(r.returncode, 0, r.stderr)
            doc = json.loads((d / 'ab.json').read_text(encoding='utf-8'))
            ab = doc['runs'][0]['ab']
            self.assertEqual(ab['windows'], {'assigned': 11, 'straddling': 1, 'unlabelled': 0})
            arms = ab['arms']
            self.assertEqual([b['block'] for b in arms['A']['blocks']], [1, 4])
            self.assertEqual([b['block'] for b in arms['B']['blocks']], [2, 3])
            self.assertEqual(arms['B']['summary']['windows'], 7)   # 5400 straddles the change at frame 5200
            self.assertEqual(arms['A']['summary']['fps_mean'], round((30.0 * 3 + 30.2) / 4, 2))
            self.assertEqual(arms['A']['spread']['fps_mean'], 0.2)
            self.assertEqual(arms['B']['spread']['over50_total'], 0.0)
            straddler = next(w for w in doc['runs'][0]['windows'] if w['frames'][1] == 5400)
            self.assertIsNone(straddler['gen'])
            self.assertIn('arm B: 2 blocks, 7 windows', r.stdout)
            r = run(BENCH / 'abtest.py', d / 'ab.json')
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)   # B is 2 fps slower, beyond tolerance and noise
            self.assertIn('arm B vs A: WORSE', r.stdout)
            self.assertRegex(r.stdout, r'fps mean .* worse \*')
            self.assertRegex(r.stdout, r'>50 ms frames /win +1 +2 .* worse \*')

    def test_abtest_noise_and_single_blocks(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            self.make_run(d, a_fps=(30.0, 27.0), b_fps=(28.0, 28.4))   # A's own blocks differ by 3 fps
            self.assertEqual(run(BENCH / 'analyze.py', d, '--arms', '--json', d / 'ab.json').returncode, 0)
            r = run(BENCH / 'abtest.py', d / 'ab.json')
            self.assertRegex(r.stdout, r'fps mean .* within noise')
            doc = json.loads((d / 'ab.json').read_text(encoding='utf-8'))
            for a in doc['runs'][0]['ab']['arms'].values():   # one block per arm: no noise estimate
                a['blocks'] = a['blocks'][:1]; a['spread'] = {k: None for k in a['spread']}
            (d / 'one.json').write_text(json.dumps(doc), encoding='utf-8')
            r = run(BENCH / 'abtest.py', d / 'one.json')
            self.assertRegex(r.stdout, r'fps mean .* worse\?')
            self.assertIn('arm B vs A: INCONCLUSIVE', r.stdout)
            self.assertEqual(r.returncode, 0)

    def test_scripts_parse(self):
        for script in ('live.sh', 'ab.sh', 'pan.sh'):
            r = subprocess.run(['sh', '-n', str(BENCH / script)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, f'{script}: {r.stderr}')
        for script in ('live.sh', 'ab.sh'):
            r = subprocess.run(['sh', str(BENCH / script), '--help'], capture_output=True, text=True)
            self.assertEqual(r.returncode, 2)
            self.assertIn('-s <serial>', r.stderr)
        # live.sh validates its lines before touching adb
        r = subprocess.run(['sh', str(BENCH / 'live.sh'), '-s', 'x', 'set', 'TOTK_VK_DESC_STOCK=on'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)
        self.assertIn('value must be a decimal', r.stderr)
        r = subprocess.run(['sh', str(BENCH / 'live.sh'), '-s', 'x', 'mark', 'a"b'], capture_output=True, text=True)
        self.assertIn('mark text', r.stderr)

    FAKE_ADB = r'''#!/bin/sh
# adb -s <serial> shell <command> with this host as the rooted device; after a control-file write the "game" answers
# with $FAKE_ANSWER in totk.log
shift 3
case $1 in
  pidof\ *) exit 0 ;;
  'su -c sh') sed "s#/data/local/tmp/#$FAKE_TMP/#g" | sh; r=$?; printf '%b' "$FAKE_ANSWER" >> "$FAKE_LOG"; exit $r ;;
  su\ -c\ *) eval "sh -c ${1#su -c }" ;;
esac
'''

    def test_live_write(self):
        """live.sh ends its batch with a newline: the game stages complete lines only."""
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / 'bin').mkdir(); (d / 'tmp').mkdir(); (d / 'game').mkdir()
            (d / 'bin/adb').write_text(self.FAKE_ADB); (d / 'bin/adb').chmod(0o755)
            (d / 'bin/chcon').write_text('#!/bin/sh\nexit 0\n'); (d / 'bin/chcon').chmod(0o755)
            log, ctl = d / 'game/totk.log', d / 'game/totk.ctl'
            log.write_text('[totk-ctl] following\n')

            def live(answer, *lines):
                env = dict(os.environ, PATH=f"{d / 'bin'}:{os.environ['PATH']}", FAKE_TMP=str(d / 'tmp'),
                           FAKE_LOG=str(log), FAKE_ANSWER=answer)
                return subprocess.run(['sh', str(BENCH / 'live.sh'), '-s', 'x', '--game-dir', str(d / 'game'),
                                       '--wait', '1', *lines], capture_output=True, text=True, env=env)

            r = live('', 'set', 'TOTK_VK_PIPE_BOOST=1', 'mark', 'a', 'b')
            self.assertEqual(r.returncode, 3, r.stderr)   # no acknowledgement
            self.assertEqual(ctl.read_text(), 'set TOTK_VK_PIPE_BOOST=1\nmark a b\n')
            r = live('[totk-ctl] gen=1 frame=9 mark ok\n', 'unset TOTK_VK_PIPE_BOOST', 'mark ok')
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn('gen=1 frame=9 mark ok', r.stdout)
            self.assertEqual(ctl.read_text(), 'set TOTK_VK_PIPE_BOOST=1\nmark a b\nunset TOTK_VK_PIPE_BOOST\nmark ok\n')
            self.assertEqual(oct(ctl.stat().st_mode & 0o777), '0o600')
            self.assertEqual(list((d / 'tmp').iterdir()), [])   # the batch's temp file is removed


class MemorySampling(unittest.TestCase):
    LINE = ('f={f} cpu=55000 gpu=60000 cool=0 tpl=0 gclk=1 gcap=2 mav={mav} rss=2000000 swp={swp} shm=1000 kgk=1048576 '
            'kgm= kgi=2097152 pcs=1.50 pcf=0.00 pms={pms} pmf=3.25 kst={kst} caps= 2016000/2016000')

    def test_kswapd_percent(self):
        import sampler
        line, st = sampler.add_kswapd(self.LINE.format(f=300, mav=1, swp=1, pms=1, kst=1000), 100.0, None)
        self.assertIn(' ksw= caps=', line)
        line, st = sampler.add_kswapd(self.LINE.format(f=300, mav=1, swp=1, pms=1, kst=1150), 103.0, st)
        self.assertIn(' ksw=50.0 caps=', line)   # 150 ticks of 100 Hz in 3 s
        self.assertEqual(st, (103.0, 1150))
        line, st = sampler.add_kswapd('f=1 cpu= kst= caps= 1/1', 106.0, st)   # no kswapd readable
        self.assertIn(' ksw= caps=', line)
        self.assertIsNone(st)

    def test_window_memory_and_old_samples(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'samples-duo.txt'
            rows = [self.LINE.format(f=f, mav=mav, swp=swp, pms=pms, kst=0) + '\n' for f, mav, swp, pms in
                    ((300, 900000, 100000, '2.00'), (300, 400000, 500000, '9.50'), (600, 350000, 600000, '1.00'))]
            p.write_text(''.join(f'{100 + i * 3}.0 {r}' for i, r in enumerate(rows))
                         + '200.0 f=600 cpu=1 gpu=2 cool=0 tpl=0 gclk=1 gcap=2 caps= 1/1\n', encoding='utf-8')   # an old-format row
            got = analyze.parse_sample_rows(p)
        self.assertEqual(len(got), 4)
        self.assertIsNone(got[3]['mav'])
        m = analyze.window_memory(600, got)   # samples taken while the window ending at 600 ran: f in [300, 600)
        self.assertEqual(m['mem_available_min_mb'], round(400000 / 1024, 1))
        self.assertEqual(m['swap_max_mb'], round(500000 / 1024, 1))
        self.assertEqual(m['psi_mem_some_max'], 9.5)
        self.assertEqual(m['kgsl_max_mb'], 3.0)   # kernel + imported, mapped absent
        self.assertNotIn('kgsl_mapped_max_mb', m)
        self.assertEqual(analyze.window_memory(300, got), {})
        self.assertEqual(analyze.worst_memory([m, {'mem_available_min_mb': 100.0, 'swap_max_mb': 1.0}])['mem_available_min_mb'], 100.0)


if __name__ == '__main__':
    unittest.main()
