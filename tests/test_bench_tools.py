"""The benchmark tools (tools/bench): the immutable baselines match their pins, the baseline write guard, the comparison
verdicts, and the ledger stays consistent with its stored results; no device needed."""
import json
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
BENCH = ROOT / 'tools/bench'
sys.path[:0] = [str(BENCH)]
import benchlib  # noqa: E402
import check_baselines  # noqa: E402


def run(*args):
    return subprocess.run([sys.executable, *map(str, args)], capture_output=True, text=True, cwd=ROOT)


class Baselines(unittest.TestCase):
    def test_baselines_match_their_pins(self):
        self.assertEqual(check_baselines.problems(), [])

    def test_analyze_and_compare_refuse_to_write_a_baseline(self):
        base = benchlib.current_baseline()
        r = run(BENCH / 'analyze.py', ROOT / 'docs', '--json', base)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('immutable', r.stderr)
        r = run(BENCH / 'analyze.py', ROOT / 'docs', '--json', base, '--new-baseline')
        self.assertIn('refusing to overwrite', r.stderr)
        r = run(BENCH / 'compare.py', base, base, '--out', ROOT / 'docs/benchmarks/baseline-test.md')
        self.assertNotEqual(r.returncode, 0)
        self.assertFalse((ROOT / 'docs/benchmarks/baseline-test.md').exists())


class Compare(unittest.TestCase):
    def setUp(self):
        self.doc = benchlib.load_result(benchlib.current_baseline())

    def test_baseline_against_itself_is_all_same(self):
        r = run(BENCH / 'compare.py', benchlib.current_baseline(), benchlib.current_baseline())
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
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
        warns = benchlib.comparability(self.doc, label, new, label)
        self.assertTrue(any('settings_env' in w for w in warns))
        self.assertTrue(any('cpufreq caps' in w for w in warns))


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


if __name__ == '__main__':
    unittest.main()
