"""GPU profile schema/analysis fixtures; optional real lavapipe matrix, no devices.

Pure suite: python -m unittest discover -s tests -p test_gpuprof.py
Real matrix (build vkgpuproftest first): GPUPROF_TEST_BIN=<absolute binary>
TOTK_SHADER_SEED_DIR=<absolute vkdraw-seed> python -m unittest discover -s tests -p test_gpuprof.py
The caller runs the matrix under the repository's taskset/nice/flock policy.
"""
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/bench'))
import gpuprof  # noqa: E402


def attachment(**changes):
    row = dict(slot=0, format=37, width_num=1, width_den=1, height_num=1, height_den=1,
               load=1, store=0)
    row.update(changes)
    return row


def counters(vertices=30, primitives=10, fragments=100):
    return dict(ia_vertices=vertices, ia_primitives=primitives, vs_invocations=vertices,
                clipping_invocations=primitives, clipping_primitives=primitives, fs_invocations=fragments)


def pass_row(**changes):
    row = dict(schema=2, type='pass', window_start=1, window_end=300, key='a', kind='render', ordinal=1,
               gpu_ms=10.0, count=2, draws=10, stats_valid=2, attachments=[attachment()], samples=1,
               shader='shader-a', stats=counters())
    row.update(changes)
    return row


def window(**changes):
    row = dict(schema=2, type='window', window_start=1, window_end=300, frames=300, partial=False,
               span_ms=12.0, measured_ms=10.0, gap_ms=2.0, dropped=0, failed_slots=0, stats_failed_slots=0)
    row.update(changes)
    return row


def frame(**changes):
    row = dict(schema=2, type='frame', frame=1, span_ms=12.0, measured_ms=10.0, gap_ms=2.0,
               dropped=0, reconciliation_failed=False)
    row.update(changes)
    return row


class Analyzer(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def file(self, name, rows, tail=''):
        path = self.root / name
        path.write_text(''.join(json.dumps(row) + '\n' for row in rows) + tail, encoding='utf-8')
        return path

    def analyze(self, rows):
        return gpuprof.analyze([self.file('gpuprof-fixture.jsonl', rows)])

    def test_sums_precede_ratios_not_ratio_averages(self):
        a = pass_row(gpu_ms=2, count=1, stats_valid=1, draws=1, stats=counters(3, 1, 100))
        b = pass_row(window_start=301, window_end=600, gpu_ms=18, count=9, stats_valid=9,
                     draws=9, stats=counters(90, 30, 200), ratios={'verts_per_draw': 999})
        report = self.analyze([window(measured_ms=2, span_ms=4), a,
                               window(window_start=301, window_end=600, measured_ms=18, span_ms=20), b])
        group = report['passes'][0]
        self.assertEqual(group['gpu_ms'], 20)
        self.assertEqual(group['stats']['ia_vertices'], 93)
        self.assertEqual(group['ratios']['verts_per_draw'], 9.3)
        self.assertEqual(group['ratios']['prims_per_draw'], 3.1)
        self.assertAlmostEqual(group['ratios']['fragments_per_prim'], 300 / 31)
        self.assertAlmostEqual(group['ratios']['ns_per_vertex'], 20e6 / 93)
        self.assertAlmostEqual(group['ratios']['ns_per_primitive'], 20e6 / 31)
        self.assertEqual(report['coverage']['window_sum_mismatches'], 0)

    def test_family_keeps_structure_ignores_shader_key_and_ordinal(self):
        rows = [pass_row(gpu_ms=1), pass_row(key='b', ordinal=2, shader='shader-b', gpu_ms=2),
                pass_row(key='c', attachments=[attachment(width_num=2)], gpu_ms=3),
                pass_row(key='d', samples=4, gpu_ms=4),
                pass_row(key='e', kind='compute', attachments=[], samples=0, gpu_ms=5)]
        report = self.analyze([window(measured_ms=15, span_ms=17), *rows])
        self.assertEqual(len(report['passes']), 5)
        self.assertEqual(len(report['families']), 4)
        merged = next(g for g in report['families'] if g['keys'] == ['a', 'b'])
        self.assertEqual(merged['gpu_ms'], 3)
        self.assertEqual(merged['ordinals'], [1, 2])
        self.assertEqual(merged['shaders'], ['shader-a', 'shader-b'])
        self.assertEqual(merged['stats']['ia_vertices'], 60)

    def test_exact_rationals_do_not_round_nearby_attachment_shapes(self):
        a = attachment(); a.update(width_num=101, width_den=100)
        b = attachment(); b.update(width_num=1001, width_den=1000)
        report = self.analyze([pass_row(attachments=[a]), pass_row(key='b', attachments=[b])])
        self.assertEqual(len(report['families']), 2)
        self.assertEqual(report['coverage']['missing_window_summaries'], 1)

    def test_attachment_order_is_not_a_family_difference(self):
        a, b = attachment(), attachment(); b['slot'] = 1
        report = self.analyze([pass_row(attachments=[a, b]), pass_row(key='b', attachments=[b, a])])
        self.assertEqual(len(report['families']), 1)

    def test_disabled_statistics_are_absent_not_synthetic_zero(self):
        row = pass_row(stats_valid=0); del row['stats']
        group = self.analyze([row])['passes'][0]
        self.assertIsNone(group['stats'])
        self.assertIsNone(group['ratios'])
        self.assertEqual(group['stats_missing'], 2)
        self.assertEqual(group['ns_per_draw'], 1e6)

    def test_nonrender_scope_kinds_have_separate_families_and_optional_stats(self):
        kinds = ('compute', 'upload', 'transfer', 'post', 'upscale', 'present', 'framegen')
        rows = [pass_row()]
        for kind in kinds:
            row = pass_row(kind=kind, key=kind, attachments=[], samples=0, stats_valid=0, count=1, draws=0)
            del row['stats']
            rows.append(row)
        report = self.analyze([window(measured_ms=80, span_ms=82), *rows])
        self.assertEqual(report['coverage']['malformed_rows'], 0)
        self.assertEqual(report['coverage']['window_sum_mismatches'], 0)
        self.assertEqual(len(report['families']), 8)
        for group in report['families']:
            if group['kind'] != 'render':
                self.assertIsNone(group['stats'])
                self.assertIsNone(group['ratios'])
        self.assertEqual(report['coverage']['stats_missing'], 7)

    def test_mixed_availability_uses_only_matched_time_and_draws(self):
        measured = pass_row(gpu_ms=2, count=1, stats_valid=1, draws=1, stats=counters(3, 1, 20))
        missing = pass_row(window_start=301, window_end=600, gpu_ms=100, count=4, stats_valid=0, draws=100)
        del missing['stats']
        group = self.analyze([measured, missing])['passes'][0]
        self.assertEqual(group['gpu_ms'], 102)
        self.assertEqual(group['stats_missing'], 4)
        self.assertEqual(group['ratios']['verts_per_draw'], 3)
        self.assertAlmostEqual(group['ratios']['ns_per_vertex'], 2e6 / 3)
        self.assertEqual(group['ratio_draws'], 1)
        self.assertEqual(group['ratio_gpu_ms'], 2)

    def test_partial_statistics_keep_counter_ratios_not_unmatched_denominators(self):
        group = self.analyze([pass_row(count=10, stats_valid=2)])['passes'][0]
        self.assertEqual(group['stats']['ia_vertices'], 30)
        self.assertEqual(group['ratios']['fragments_per_prim'], 10)
        self.assertIsNone(group['ratios']['verts_per_draw'])
        self.assertIsNone(group['ratios']['ns_per_vertex'])
        self.assertEqual(group['stats_partial_rows'], 1)
        self.assertEqual(group['stats_missing'], 8)

    def test_zero_denominators_are_unavailable(self):
        group = self.analyze([pass_row(draws=0, stats=counters(0, 0, 0))])['passes'][0]
        self.assertTrue(all(value is None for value in group['ratios'].values()))
        self.assertIsNone(group['ns_per_draw'])

    def test_long_frame_population_is_never_double_counted(self):
        selected = pass_row(type='long_frame', frame=12, gpu_ms=99)
        del selected['window_start']; del selected['window_end']
        report = self.analyze([window(), pass_row(), frame(), selected])
        self.assertEqual(report['passes'][0]['gpu_ms'], 10)
        self.assertEqual(report['families'][0]['gpu_ms'], 10)
        self.assertEqual(report['long_frame_passes'][0]['gpu_ms'], 99)
        self.assertEqual(report['coverage']['long_frames'], 1)
        self.assertEqual(report['timing']['window']['measured_ms'], 10)
        self.assertEqual(report['timing']['frame']['measured_ms'], 10)

    def test_long_frame_history_does_not_impose_a_dump_limit(self):
        rows = [pass_row(type='long_frame', frame=n, count=1, stats_valid=1, gpu_ms=n) for n in range(1, 10)]
        report = self.analyze(rows)
        self.assertEqual(report['coverage']['long_frames'], 9)
        self.assertEqual(report['long_frame_passes'][0]['gpu_ms'], 45)
        self.assertEqual(report['passes'], [])

    def test_censoring_frame_gaps_and_reconciliation_are_explicit(self):
        report = self.analyze([window(dropped=3, failed_slots=1, stats_failed_slots=2, partial=True), pass_row(),
                               frame(), frame(frame=4, dropped=2, reconciliation_failed=True),
                               frame(frame=5, gap_ms=0)])
        coverage = report['coverage']
        self.assertEqual(coverage['censored_windows'], 1)
        self.assertEqual(coverage['censored_frames'], 1)
        self.assertEqual(coverage['missing_frame_ids'], 2)
        self.assertEqual(coverage['reconciliation_failures'], 1)
        self.assertEqual(coverage['dropped'], 3)
        self.assertEqual(coverage['failed_slots'], 1)
        self.assertEqual(coverage['stats_failed_slots'], 2)
        self.assertEqual(report['timing']['frame']['gap_ms'], 4)

    def test_missing_rotated_pass_rows_are_censored(self):
        report = self.analyze([window(), pass_row(gpu_ms=4)])
        self.assertEqual(report['coverage']['window_sum_mismatches'], 1)
        self.assertTrue(any('censored' in issue for issue in report['issues']))

    def test_rotation_globs_duplicate_inputs_and_duplicate_rows(self):
        self.file('gpuprof-run.jsonl.3', [window(), pass_row()])
        current = self.file('gpuprof-run.jsonl', [pass_row(), frame()])
        report = gpuprof.analyze([str(self.root / '*.jsonl*'), current])
        self.assertEqual(len(report['files']), 2)
        self.assertEqual(report['coverage']['streams'], 1)
        self.assertEqual(report['coverage']['duplicate_rows'], 1)
        self.assertEqual(report['passes'][0]['gpu_ms'], 10)
        self.assertEqual(report['coverage']['window_sum_mismatches'], 0)

    def test_separate_launches_keep_identical_window_observations(self):
        a = self.file('gpuprof-one.jsonl', [window(), pass_row()])
        b = self.file('gpuprof-two.jsonl', [window(), pass_row()])
        report = gpuprof.analyze([a, b])
        self.assertEqual(report['coverage']['streams'], 2)
        self.assertEqual(report['coverage']['duplicate_rows'], 0)
        self.assertEqual(report['passes'][0]['gpu_ms'], 20)

    def test_conflicts_exclude_both_not_arbitrary_first_wins(self):
        report = self.analyze([window(), pass_row(), pass_row(gpu_ms=999)])
        self.assertEqual(report['coverage']['conflicting_rows'], 1)
        self.assertEqual(report['passes'], [])
        self.assertEqual(report['coverage']['window_sum_mismatches'], 1)

    def test_malformed_rows_and_incomplete_rotation_tail_are_reported(self):
        path = self.file('gpuprof-run.jsonl', [window(), pass_row()], '\nnot json\n{"schema":2')
        report = gpuprof.analyze([path])
        self.assertEqual(report['coverage']['malformed_rows'], 1)
        self.assertEqual(report['coverage']['truncated_rows'], 1)
        self.assertEqual(report['coverage']['blank_lines'], 1)
        self.assertEqual(report['coverage']['pass_rows'], 1)

    def test_invalid_schema_fields_are_rejected_without_losing_valid_rows(self):
        invalid = []
        for changes in ({'schema': 1}, {'gpu_ms': -1}, {'gpu_ms': float('nan')}, {'gpu_ms': float('inf')},
                        {'draws': True}, {'stats_valid': 3}, {'stats': None}, {'type': 'other'},
                        {'attachments': [{**attachment(), 'width_den': 0}]},
                        {'attachments': [{**attachment(), 'width_num': 2, 'width_den': 2}]},
                        {'attachments': [attachment(), attachment()]}, {'ratios': {'ns_per_vertex': -1}}):
            invalid.append(pass_row(**changes))
        report = self.analyze([*invalid, pass_row()])
        self.assertEqual(report['coverage']['malformed_rows'], len(invalid))
        self.assertEqual(report['coverage']['pass_rows'], 1)

    def test_invalid_stats_presence_and_incomplete_stats_are_rejected(self):
        missing = pass_row(); del missing['stats']
        zero = pass_row(stats_valid=0)
        incomplete = pass_row(stats={k: v for k, v in counters().items() if k != 'fs_invocations'})
        self.assertEqual(self.analyze([missing, zero, incomplete])['coverage']['malformed_rows'], 3)

    def test_text_and_json_cli_and_input_errors(self):
        path = self.file('gpuprof-run.jsonl', [window(), pass_row()])
        command = [sys.executable, str(ROOT / 'tools/bench/gpuprof.py')]
        result = subprocess.run([*command, str(path), '--json', '-'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['passes'][0]['key'], 'a')
        output = self.root / 'analysis.json'
        result = subprocess.run([*command, str(path), '--json', str(output)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('Structural families', result.stdout)
        self.assertIn('stats valid/missing', result.stdout)
        self.assertTrue(output.is_file())
        for arguments in ([str(self.root / 'absent*')], [str(path), '--json', str(path)]):
            result = subprocess.run([*command, *arguments], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(json.loads(path.read_text().splitlines()[0])['schema'], 2)

    def test_cli_malformed_interior_is_failure_but_partial_tail_is_warning(self):
        command = [sys.executable, str(ROOT / 'tools/bench/gpuprof.py')]
        for tail, status in [('bad\n', 1), ('{"schema":', 0)]:
            path = self.file('gpuprof-run.jsonl', [pass_row()], tail)
            result = subprocess.run([*command, str(path)], capture_output=True, text=True)
            self.assertEqual(result.returncode, status, result.stderr)
            self.assertIn('WARNING:', result.stdout)

    def test_render_includes_all_counters_and_missing_data_not_zero_ratios(self):
        row = pass_row(stats_valid=0); del row['stats']
        text = io.StringIO(); gpuprof.render(self.analyze([row]), text)
        self.assertIn('counters: unavailable', text.getvalue())
        self.assertIn(' | - | - | - | - | -', text.getvalue())
        text = io.StringIO(); gpuprof.render(self.analyze([pass_row()]), text)
        for key in gpuprof.STATS:
            self.assertIn(key + '=', text.getvalue())


@unittest.skipUnless(os.environ.get('GPUPROF_TEST_BIN'), 'set GPUPROF_TEST_BIN to run the real Vulkan fixture')
class Lavapipe(unittest.TestCase):
    def launch(self, mode, workers, stats, *, unsupported=False, rotate=False):
        binary = Path(os.environ['GPUPROF_TEST_BIN']).resolve()
        self.assertTrue(binary.is_file(), f'no fixture binary: {binary}')
        seed = Path(os.environ.get('TOTK_SHADER_SEED_DIR', binary.parent / 'vkdraw-seed')).resolve()
        self.assertTrue(seed.is_dir(), f'no shader seed: {seed}')
        with tempfile.TemporaryDirectory(prefix='gpuprof-test-') as temporary:
            env = dict(os.environ, TOTK_SHADER_SEED_DIR=str(seed), TOTK_VK_GPU_PROFILE=str(mode),
                       TOTK_VK_REC_THREADS=str(workers), TOTK_VK_GPU_PROFILE_STATS=str(stats),
                       TOTK_VK_GPU_PROFILE_LONG_MS='0', TOTK_VK_GPU_PROFILE_LONG_FRAMES='2',
                       TOTK_VK_GPU_PROFILE_MAX_BYTES='4096' if rotate else '16777216')
            # Isolate launch policy from a developer's shell diagnostics.
            for key in ('TOTK_VK_OCCLUSION', 'TOTK_VK_PASS_DUMP', 'TOTK_VK_PASS_PROFILE', 'TOTK_VK_PASSPROBE',
                        'TOTK_VK_PASS_MERGE', 'GPUPROF_TEST_UNSUPPORTED', 'TOTK_VK_OFF', 'TOTK_VK_WRITEBACK',
                        'TOTK_FRAME_LAG', 'TOTK_VK_LAG_CAPTURE'):
                env.pop(key, None)
            if unsupported:
                env['GPUPROF_TEST_UNSUPPORTED'] = '1'
            result = subprocess.run([str(binary), '--frames', '302'], cwd=temporary, env=env,
                                    capture_output=True, text=True, timeout=180)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            match = re.search(r'gpuprof-fixture frames=(\d+) passes=(\d+) draws=(\d+) mode=(\d+) workers=(\d+) '
                              r'stats_supported=(\d+) stats_enabled=(\d+)', result.stdout)
            self.assertIsNotNone(match, result.stdout + result.stderr)
            frames, passes, draws, _, _, supported, enabled = map(int, match.groups())
            paths = sorted(Path(temporary).glob('gpuprof-*.jsonl*'))
            if not mode:
                self.assertEqual(paths, [], 'off mode must not create a profiler output file')
                return None
            self.assertTrue(paths, result.stdout + result.stderr)
            report = gpuprof.analyze(paths)
            self.assertEqual(report['coverage']['malformed_rows'], 0, report['issues'])
            self.assertEqual(report['coverage']['truncated_rows'], 0, report['issues'])
            self.assertEqual(report['coverage']['conflicting_rows'], 0, report['issues'])
            observations, _, _ = gpuprof.load(gpuprof.inputs(paths))
            if rotate:
                self.assertEqual({p.suffix for p in paths}, {'.jsonl', '.1', '.2', '.3'})
                self.assertTrue(all(p.stat().st_size <= 4096 for p in paths))
                self.assertLess(report['coverage']['frame_rows'], frames, 'bounded rotation must discard old rows')
                return report
            self.assertEqual(report['coverage']['frame_rows'], frames)
            self.assertEqual(report['coverage']['windows'], 2)
            self.assertEqual(report['coverage']['window_sum_mismatches'], 0, report['issues'])
            self.assertEqual(report['coverage']['missing_frame_ids'], 0)
            self.assertEqual(report['coverage']['reconciliation_failures'], 0, report['issues'])
            self.assertEqual(report['coverage']['dropped'], 0)
            self.assertEqual(report['coverage']['failed_slots'], 0)
            render_groups = [g for g in report['passes'] if g['kind'] == 'render']
            other_groups = [g for g in report['passes'] if g['kind'] != 'render']
            self.assertEqual(sum(g['count'] for g in render_groups), passes)
            self.assertEqual(sum(g['draws'] for g in render_groups), draws)
            self.assertGreater(sum(g['gpu_ms'] for g in render_groups), 0)
            self.assertEqual(len({g['key'] for g in render_groups}), 1,
                             'draw-count buckets, code addresses and absolute extent must not change render keys')
            self.assertGreaterEqual(sum(g['count'] for g in other_groups if g['kind'] == 'transfer'), passes,
                                    'each real readback must have a measured transfer scope')
            self.assertTrue(any(g['kind'] == 'upload' for g in other_groups),
                            'direct pre upload while a main record is open must survive')
            self.assertAlmostEqual(report['timing']['window']['measured_ms'],
                                   report['timing']['frame']['measured_ms'], delta=0.01,
                                   msg='all measured kinds must reconcile, not only render rows')
            self.assertEqual(bool(enabled), bool(stats and supported and not unsupported))
            for observation in observations:
                row = observation['row']
                if row['type'] in ('pass', 'long_frame'):
                    available = bool(enabled and row['kind'] == 'render')
                    self.assertEqual('stats' in row, available)
                    self.assertEqual(row['stats_valid'], row['count'] if available else 0)
                    if available:
                        self.assertEqual(row['stats']['ia_vertices'], row['draws'] * 3)
                        self.assertEqual(row['stats']['ia_primitives'], row['draws'])
                        self.assertGreater(row['stats']['fs_invocations'], 0)
                if row['type'] == 'frame':
                    self.assertFalse(row['reconciliation_failed'])
                    self.assertEqual(row['dropped'], 0)
            missing = sum(g['count'] for g in other_groups) + (0 if enabled else passes)
            self.assertEqual(report['coverage']['stats_missing'], missing)
            if mode == 3:
                self.assertEqual(report['coverage']['long_frames'], frames,
                                 'threshold zero must dump every frame, independent of history ring retention')
            else:
                self.assertEqual(report['coverage']['long_frames'], 0)
            return report

    def test_modes_workers_statistics_and_stable_keys(self):
        reference = None
        for mode in range(4):
            for workers in (0, 2):
                for stats in (0, 1):
                    with self.subTest(mode=mode, workers=workers, stats=stats):
                        report = self.launch(mode, workers, stats)
                        if report:
                            keys = {(g['key'], g['shader']) for g in report['passes'] if g['kind'] == 'render'}
                            if reference is None:
                                reference = keys
                            self.assertEqual(keys, reference, 'recorder/mode/stats must not change pass keys')
        for workers in (0, 2):
            with self.subTest(unsupported=True, workers=workers):
                report = self.launch(1, workers, 1, unsupported=True)
                self.assertEqual({(g['key'], g['shader']) for g in report['passes'] if g['kind'] == 'render'}, reference)

    def test_bounded_output_rotation_retains_whole_schema_rows(self):
        self.launch(3, 2, 1, rotate=True)


if __name__ == '__main__':
    unittest.main()
