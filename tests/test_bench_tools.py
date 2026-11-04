"""The benchmark tools (tools/bench): the immutable baselines match their pins, the baseline write guard, frequency
limits and signatures, automatic per-signature baselines, the comparison verdicts and signature checks, and the ledger
stays consistent with its stored results; no device needed."""
import io
import json
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


if __name__ == '__main__':
    unittest.main()
