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
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BENCH = ROOT / 'tools/bench'
sys.path[:0] = [str(BENCH)]
import analyze  # noqa: E402
import abtest  # noqa: E402
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


class Pacing(unittest.TestCase):
    def test_steady_and_dip_report_separate_time_weighted_states(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / 'logs').mkdir()
            (d / 'logs/odin.log').write_text(ab_log(
                {600: (57, 20, 10, 0), 900: (34, 34, 10, 0)}, []), encoding='utf-8')
            (d / 'settings-odin.env').write_text('TOTK_FPS60=1\n')
            (d / 'pans.txt').write_text('odin hateno F0=0 F1=900 T0=100 T1=120 V=16000\n')
            result = run(BENCH / 'analyze.py', d, '--json', d / 'result.json')
            self.assertEqual(result.returncode, 0, result.stderr)
            record = json.loads((d / 'result.json').read_text())['runs'][0]
        s = record['summary']
        self.assertEqual(s['fps_steady'], 57)
        self.assertEqual(s['fps_mean'], 45.5)
        self.assertEqual(s['dip_windows'], 1)
        self.assertAlmostEqual(s['dip_time_fraction'], (300 / 34) / (300 / 57 + 300 / 34), places=6)
        self.assertIsNone(s['frames_near33'])  # fps cannot recover individual frame counts
        self.assertEqual([w['pacing_source'] for w in record['windows']], ['window-fps'] * 2)

    def test_raw_durations_and_fence_histogram_precede_window_fps(self):
        raw = {f: 33.33 if f <= 75 else 16.67 for f in range(1, 301)}
        w = analyze.pacing_of(300, 51, 60, {'frame_ms': raw})
        self.assertEqual((w['pacing_state'], w['frames_near33']), ('dip', 75))
        self.assertEqual(w['pacing_source'], 'frame-durations')
        self.assertEqual(analyze.pacing_of(300, 30, 30, {'frame_ms': raw})['pacing_state'], 'steady')
        # Incomplete traces are not evidence that the unlogged frames were steady.
        w = analyze.pacing_of(300, 34, 60, {'frame_ms': {1: 33.3}})
        self.assertEqual((w['pacing_state'], w['pacing_source']), ('dip', 'window-fps'))
        self.assertEqual(w['frames_near33'], 1)
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'totk.log'
            p.write_text('[nvn-vk] frame 300: 51.0 fps over the last 300, 1000 draws\n'
                         '[nvn-vk] window present-fence interval bins ms '
                         '[<12.5,12.5-20.9,20.9-29,29-37.5,37.5-45.8,>=45.8]: '
                         'all=0/225/0/75/0/0; adjacent immutable interval2=0/0/0/70/0/0\n')
            info = analyze.parse_log(p)[5]
        w = analyze.pacing_of(300, 51, 60, info)
        self.assertEqual((w['pacing_state'], w['frames_near33_source']), ('dip', 'present-fence-histogram'))
        self.assertEqual(w['frames_near33'], 75)


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
        self.assertEqual([g['direction'] for g in gens], ['right', 'left', 'right', 'left', None])

    def test_explicit_pan_direction(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'x.log'
            p.write_text('[totk-ctl] gen=1 frame=1000 mark ab arm=A block=1 direction=left\n'
                         '[totk-ctl] gen=2 frame=2000 mark ab arm=B direction=right\n', encoding='utf-8')
            gens = analyze.parse_ctl(p)
        self.assertEqual([g['direction'] for g in gens], ['left', 'right'])
        self.assertIsNone(analyze.pan_direction(None))

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
            self.assertEqual([b['direction'] for b in arms['A']['blocks']], ['right', 'left'])
            self.assertEqual([g['direction'] for g in ab['generations']], ['right', 'left', 'right', 'left', None])
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
            self.assertIn('sign agreement 2/2 (ties 0); exact sign p=0.5000', r.stdout)
            self.assertRegex(r.stdout, r'>50 ms frames /win +1 +2 .* worse \*')

    def test_abtest_mixed_signs_and_single_blocks(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            self.make_run(d, a_fps=(30.0, 27.0), b_fps=(28.0, 28.4))   # A's own blocks differ by 3 fps
            self.assertEqual(run(BENCH / 'analyze.py', d, '--arms', '--json', d / 'ab.json').returncode, 0)
            r = run(BENCH / 'abtest.py', d / 'ab.json')
            self.assertRegex(r.stdout, r'fps mean .* same \*')
            self.assertIn('sign agreement 1/2 (ties 0); exact sign p=1.0000', r.stdout)
            doc = json.loads((d / 'ab.json').read_text(encoding='utf-8'))
            for a in doc['runs'][0]['ab']['arms'].values():   # incomplete quartet: no direction-matched pairs
                a['blocks'] = a['blocks'][:1]; a['spread'] = {k: None for k in a['spread']}
            (d / 'one.json').write_text(json.dumps(doc), encoding='utf-8')
            r = run(BENCH / 'abtest.py', d / 'one.json')
            self.assertRegex(r.stdout, r'fps mean .* worse\?')
            self.assertIn('arm B vs A: INCONCLUSIVE', r.stdout)
            self.assertEqual(r.returncode, 0)

    @staticmethod
    def records(a_rows, b_rows):
        """Compact block-summary fixtures; old runs intentionally omit direction metadata."""
        keys = ('windows', 'fps_mean', 'fps_min', 'p50_ms_median', 'p95_ms_max', 'max_ms', 'gpu_ms_mean',
                'over50_total', 'long_ge45_total')
        arms = []
        for positions, rows in (((1, 4), a_rows), ((2, 3), b_rows)):
            blocks, windows = [], []
            for i, row in enumerate(rows):
                summary = dict(zip(keys, row))
                blocks.append({'block': 4 * (i // 2) + positions[i % 2], 'summary': summary})
                windows += [dict(fps=summary['fps_mean'], p50_ms=summary['p50_ms_median'],
                                 p95_ms=summary['p95_ms_max'], max_ms=summary['max_ms'], gpu_ms=summary['gpu_ms_mean'],
                                 over50=summary['over50_total'] / summary['windows'],
                                 long_ge45=summary['long_ge45_total'] / summary['windows'])] * summary['windows']
            spreads = {}
            for key in analyze.AB_METRICS:
                values = [value for b in blocks if (value := analyze.block_metric(b['summary'], key)) is not None]
                spreads[key] = max(values) - min(values) if len(values) > 1 else None
            pooled = analyze.summarize(windows)
            pooled['fps_min'] = min(b['summary']['fps_min'] for b in blocks)
            arms.append({'blocks': blocks, 'summary': pooled, 'spread': spreads})
        return arms

    def fps_pairs(self, deltas):
        """Each quartet has a 20 fps view-content effect and two B-A deltas (right, left)."""
        a, b = [], []
        for right, left in zip(deltas[::2], deltas[1::2]):
            a += [[1, fps, 20, 20, 30, 40, 10, 0, 0] for fps in (30, 50)]
            b += [[1, fps, 20, 20, 30, 40, 10, 0, 0] for fps in (50 + left, 30 + right)]
        return self.records(a, b)

    def test_direction_confounded_win_and_loss(self):
        for delta, expected in ((10, 'better'), (-10, 'worse'), (0, 'same')):
            with self.subTest(delta=delta):
                ref, arm = self.fps_pairs([delta] * 4)
                stats = abtest.paired_metric('fps_mean', ref, arm)
                self.assertEqual([(p['reference_block'], p['arm_block']) for p in stats['pairs']],
                                 [(1, 3), (4, 2), (5, 7), (8, 6)])
                self.assertEqual(stats['delta'], delta)
                self.assertEqual(stats['directions'], {'right': delta, 'left': delta})
                self.assertEqual(abtest.metric_verdict('fps_mean', ref, arm)[3], expected)
                self.assertEqual(abtest.arm_verdict({k: abtest.metric_verdict(k, ref, arm)
                                                    for k in analyze.AB_METRICS}), expected)
                self.assertGreater(ref['spread']['fps_mean'], abs(delta))
                if delta:
                    self.assertEqual(abtest.metric_verdict('fps_mean', arm, ref)[3],
                                     'worse' if delta > 0 else 'better')

    def test_both_pooled_and_paired_delta_must_cross_tolerance(self):
        for delta in (10, -10):
            ref, arm = self.fps_pairs([delta] * 4)
            arm['summary']['fps_mean'] = ref['summary']['fps_mean']
            expected = 'lean better' if delta > 0 else 'lean worse'
            self.assertEqual(abtest.metric_verdict('fps_mean', ref, arm)[3], expected)
            self.assertEqual(abtest.arm_verdict({'fps_mean': abtest.metric_verdict('fps_mean', ref, arm)}), 'same')
            arm['summary']['fps_mean'] -= delta
            self.assertEqual(abtest.metric_verdict('fps_mean', ref, arm)[3], expected)  # opposite pooled sign
        ref, arm = self.fps_pairs([0] * 4)
        arm['summary']['fps_mean'] += 10
        self.assertEqual(abtest.metric_verdict('fps_mean', ref, arm)[3], 'same')

    def test_sign_test_and_unanimity(self):
        for deltas, expected, probability in (
                ([4, 4, 4, 0], 'same', 0.25),       # a tie prevents unanimous confirmation
                ([4, 4, 4, -1], 'same', 0.625),     # mixed signs, too few pairs
                ([4] * 7 + [-1], 'same', 0.0703125),
                ([4] * 9 + [-1], 'better', 0.021484375),
                ([-4] * 9 + [1], 'worse', 0.021484375)):
            with self.subTest(deltas=deltas):
                ref, arm = self.fps_pairs(deltas)
                stats = abtest.paired_metric('fps_mean', ref, arm)
                self.assertAlmostEqual(stats['p_value'], probability)
                self.assertEqual(abtest.metric_verdict('fps_mean', ref, arm)[3], expected)

    def test_pairs_require_quartet_and_matching_direction(self):
        ref, arm = self.fps_pairs([10] * 4)
        # Explicit direction wins over parity; mismatched directions cannot form a pair.
        arm['blocks'][1]['direction'] = 'left'
        stats = abtest.paired_metric('fps_mean', ref, arm)
        self.assertEqual(len(stats['pairs']), 3)
        ref['blocks'].pop()  # do not pair across the resulting incomplete quartet
        self.assertEqual(len(abtest.paired_metric('fps_mean', ref, arm)['pairs']), 1)
        self.assertEqual(abtest.metric_verdict('fps_mean', ref, arm)[3], 'better?')
        ref, arm = self.fps_pairs([10] * 4)
        for record in (ref, arm):
            for block in record['blocks']:
                block['block'] = None
        self.assertEqual(abtest.paired_metric('fps_mean', ref, arm)['pairs'], [])

    def test_odin_plan1_regressions(self):
        # Observed block summaries from .scratch/bench/live/odin-ab-e2d3609/<name>/ab.json.
        # Preserve windows/counts and all compared metrics, not only the headline fps values.
        cases = (
            ('ab1-split512', 'better', (22.58, 18.06),
             [[3, 38.63, 36.6, 22.58, 37.33, 42.7, 15.83, 0, 0],
              [4, 44.55, 34.8, 31.4, 37.79, 43.4, 14.95, 0, 0],
              [3, 37.1, 33.7, 28.88, 37.44, 44.7, 16.07, 0, 0],
              [4, 47.17, 38.4, 21.39, 37.37, 41.8, 14.43, 0, 0]],
             [[5, 54.18, 50.4, 18.27, 32.59, 37.7, 13.94, 0, 0],
              [4, 48.17, 40, 19.8, 38.77, 95.4, 14.6, 1, 1],
              [5, 51.16, 35.6, 17.93, 37.34, 42.7, 14.46, 0, 0],
              [4, 55.28, 54, 17.96, 21.63, 37.9, 13.88, 0, 0]]),
            ('ab2-prepmask', 'same', (20.64, 21.1),
             [[3, 37.6, 33.9, 22.02, 37.44, 48, 16.07, 0, 2],
              [4, 45.05, 31.8, 20.23, 37.44, 41.7, 14.8, 0, 0],
              [3, 41.43, 36.2, 27.34, 37.64, 52.5, 15.43, 2, 3],
              [4, 47.75, 38.6, 20.06, 37.02, 41.3, 14.4, 0, 0]],
             [[3, 42.37, 32.9, 20.4, 37.21, 40.3, 15.27, 0, 0],
              [3, 39.6, 34.1, 30.55, 37.43, 43, 15.8, 0, 0],
              [4, 48.05, 37, 20.42, 37.21, 40.2, 14.47, 0, 0],
              [3, 39.77, 36.4, 21.3, 36.82, 55, 15.47, 4, 8]]),
            ('ab6-lookout-split512', 'same', (18.04, 18.15),
             [[4, 53.12, 48.6, 18.23, 33.72, 40.3, 13.53, 0, 0],
              [5, 53.56, 49.8, 18.04, 22.78, 38, 13.72, 0, 0],
              [5, 53.64, 49, 18.12, 30.46, 37.4, 13.68, 0, 0],
              [5, 51.42, 45.2, 17.88, 34.53, 40.5, 13.66, 0, 0]],
             [[4, 54.6, 50.3, 18.15, 33.77, 39.5, 13.35, 0, 0],
              [4, 52.4, 48.5, 18.46, 32.74, 40.4, 13.72, 0, 0],
              [4, 52.75, 46.9, 18.34, 34.61, 37.2, 13.57, 0, 0],
              [4, 50.42, 44.8, 19.03, 36.35, 42.8, 13.88, 0, 0]]))
        for name, expected, medians, a_rows, b_rows in cases:
            with self.subTest(run=name):
                ref, arm = self.records(a_rows, b_rows)
                # A pooled median cannot be reconstructed from block medians: retain the observed pooled values.
                ref['summary']['p50_ms_median'], arm['summary']['p50_ms_median'] = medians
                per = {k: abtest.metric_verdict(k, ref, arm) for k in analyze.AB_METRICS}
                self.assertEqual(abtest.arm_verdict(per), expected, per)
                if name == 'ab6-lookout-split512':
                    self.assertEqual(per['p50_ms_median'][3], 'lean worse')
                with tempfile.TemporaryDirectory() as tmp:
                    path = Path(tmp) / 'ab.json'
                    path.write_text(json.dumps({'runs': [{'device': 'odin', 'scene': name, 'ab': {
                        'windows': {'assigned': ref['summary']['windows'] + arm['summary']['windows'],
                                    'straddling': 0, 'unlabelled': 0}, 'arms': {'A': ref, 'B': arm}}}]}),
                                    encoding='utf-8')
                    result = run(BENCH / 'abtest.py', path)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn(f'arm B vs A: {expected.upper()}', result.stdout)
                    if name == 'ab1-split512':
                        self.assertIn('direction Δ: right +13.86, left +6.81; sign agreement 4/4', result.stdout)

    def test_ab_driver_records_alternating_direction(self):
        # Exercise the host driver with stand-in scripts only: never touch adb or a device.
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / 'ab.sh').write_text((BENCH / 'ab.sh').read_text(encoding='utf-8'), encoding='utf-8')
            for script, text in (
                    ('live.sh', '#!/bin/sh\nfor line do printf "[totk-ctl] gen=1 frame=1000 %s\\n" "$line"; done\n'),
                    ('pan.sh', '#!/bin/sh\nprintf "pan args: %s\\n" "$*"\n'),
                    ('sleep', '#!/bin/sh\nexit 0\n')):
                (d / script).write_text(text, encoding='utf-8')
                (d / script).chmod(0o755)
            result = subprocess.run(['sh', str(d / 'ab.sh'), '-s', 'fake', '--device', 'test', '--scene', 'hateno',
                                     '--arms', 'A:unset TOTK_VK_DESC_STOCK;B:set TOTK_VK_DESC_STOCK=1',
                                     '--order', 'ABBA', '--settle', '0', '--event', '/fake', '--display', '0',
                                     '--out', str(d / 'out')],
                                    env={**os.environ, 'PATH': str(d) + os.pathsep + os.environ['PATH']},
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            lines = (d / 'out/arms.txt').read_text(encoding='utf-8').splitlines()
            self.assertEqual(len(lines), 4)
            for index, direction in enumerate(('right', 'left', 'right', 'left'), 1):
                self.assertIn(f'block={index} arm={"ABBA"[index - 1]} direction={direction} gen=1 frame=1000', lines[index - 1])
            pans = [line for line in result.stdout.splitlines() if line.startswith('pan args:')]
            self.assertEqual(len(pans), 4)
            for line, direction in zip(pans, ('right', 'left', 'right', 'left')):
                self.assertIn(f'--first {direction} ', line)

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

    def test_live_write(self):
        """live.sh ends its batch with a newline (the game stages complete lines only), reports refusals, and all its
        root commands share one root shell."""
        with fake_device() as dev:
            log, ctl = dev.game / 'totk.log', dev.game / 'totk.ctl'
            log.write_text('[totk-ctl] following\n')
            stop = threading.Event()
            game = threading.Thread(target=fake_game, args=(ctl, log, stop), daemon=True)
            game.start()
            try:
                def live(*lines):
                    return subprocess.run(['sh', str(BENCH / 'live.sh'), '-s', 'x', '--game-dir', str(dev.game),
                                           '--wait', '5', *lines], capture_output=True, text=True, env=dev.env)
                r = live('set', 'TOTK_VK_PIPE_BOOST=1', 'mark', 'a', 'b')
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn('gen=1 frame=10 mark a b', r.stdout)
                self.assertEqual(ctl.read_text(), 'set TOTK_VK_PIPE_BOOST=1\nmark a b\n')
                r = live('unset TOTK_VK_PIPE_BOOST', 'mark ok')
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn('gen=2 frame=20 mark ok', r.stdout)
                self.assertEqual(ctl.read_text(), 'set TOTK_VK_PIPE_BOOST=1\nmark a b\nunset TOTK_VK_PIPE_BOOST\nmark ok\n')
                self.assertEqual(oct(ctl.stat().st_mode & 0o777), '0o600')
                r = live('set', 'TOTK_X=1', 'mark', 'r')
                self.assertEqual(r.returncode, 4, r.stderr)
                self.assertIn('refused "set TOTK_X=1"', r.stdout)
            finally:
                stop.set()
            self.assertEqual(dev.root_shells(), 1)


FAKE_ADB = r'''#!/bin/sh
# adb -s <serial> shell <command> with this host as the rooted device; every call is logged to $FAKE_CALLS
echo "$*" >> "$FAKE_CALLS"
shift 3
case $1 in
  pidof) exit 0 ;;
  'su -c sh') exec sh ;;
  *) echo "fake adb: unexpected $1" >&2; exit 1 ;;
esac
'''


class fake_device:
    """A temp dir with a fake adb (and chcon) on PATH and its own rootsh.py socket dir; stops the broker on exit."""

    def __enter__(self):
        self.tmp = tempfile.TemporaryDirectory()
        d = self.dir = Path(self.tmp.name)
        (d / 'bin').mkdir(); (d / 'game').mkdir()
        for name, text in (('adb', FAKE_ADB), ('chcon', '#!/bin/sh\nexit 0\n')):
            (d / 'bin' / name).write_text(text); (d / 'bin' / name).chmod(0o755)
        self.game, self.calls = d / 'game', d / 'calls'
        self.env = dict(os.environ, PATH=f"{d / 'bin'}:{os.environ['PATH']}", FAKE_CALLS=str(self.calls),
                        XDG_RUNTIME_DIR=str(d))
        return self

    def rootsh(self, *args, stdin=None):
        return subprocess.run([sys.executable, str(BENCH / 'rootsh.py'), '-s', 'x', *args], input=stdin,
                              capture_output=True, env=self.env)

    def root_shells(self):
        return self.calls.read_text().count('su -c sh') if self.calls.exists() else 0

    def __exit__(self, *exc):
        self.rootsh('stop')
        self.tmp.cleanup()


def fake_game(ctl, log, stop):
    """The game's side of totk.ctl: each poll that finds new complete lines is one generation."""
    seen, gen = 0, 0
    while not stop.is_set():
        data = ctl.read_bytes() if ctl.exists() else b''
        end = data.rfind(b'\n') + 1
        if end > seen:
            gen += 1
            out = []
            for line in data[seen:end].decode().splitlines():
                if line.startswith('set TOTK_X'):
                    out.append(f'[totk-ctl] refused "{line}": TOTK_X is not a live switch')
                elif line.startswith('mark '):
                    out.append(f'[totk-ctl] gen={gen} frame={gen * 10} {line}')
                else:
                    out.append(f'[totk-ctl] gen={gen} frame={gen * 10} applied {line.removeprefix("set ")}')
            seen = end
            with log.open('a') as f:
                f.write(''.join(x + '\n' for x in out))
        time.sleep(0.02)


class RootShell(unittest.TestCase):
    def test_broker(self):
        """One root shell serves every request; each script's output, status, exit and syntax errors stay its own."""
        with fake_device() as dev:
            r = dev.rootsh('run', '-c', 'echo out; echo err >&2; exit 3')
            self.assertEqual((r.returncode, r.stdout, r.stderr), (3, b'out\n', b'err\n'))
            r = dev.rootsh('run', '-c', 'if then')   # a syntax error ends only that script
            self.assertNotEqual(r.returncode, 0)
            r = dev.rootsh('run', stdin=b"printf '\\000\\377abc'")   # binary output, no final newline
            self.assertEqual((r.returncode, r.stdout), (0, b'\x00\xffabc'))
            r = dev.rootsh('run', '-c', 'cat; echo done')   # stdin is /dev/null, not the root shell's
            self.assertEqual((r.returncode, r.stdout), (0, b'done\n'))
            self.assertRegex(dev.rootsh('status').stdout.decode(), r'^rootsh pid=\d+ cpus=')
            self.assertEqual(dev.root_shells(), 1)
            self.assertEqual(dev.rootsh('stop').returncode, 0)
            self.assertFalse((dev.dir / 'totk-rootsh-x.sock').exists())
            self.assertIn(b'not running', dev.rootsh('status').stdout)


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
