"""GPU profile schema/analysis fixtures; optional real lavapipe matrix, no devices.

Pure suite: python -m unittest discover -s tests -p test_gpuprof.py
Real matrix (build vkgpuproftest first): GPUPROF_TEST_BIN=<absolute binary>
TOTK_SHADER_SEED_DIR=<absolute vkdraw-seed> python -m unittest discover -s tests -p test_gpuprof.py
The caller runs the matrix under the repository's taskset/nice/flock policy.
"""
import io
import json
import math
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


def attachment(width=1280, height=720, reference_width=1280, reference_height=720, **changes):
    width_gcd, height_gcd = math.gcd(width, reference_width), math.gcd(height, reference_height)
    row = dict(slot=0, format=37, width=width, height=height,
               width_num=width // width_gcd, width_den=reference_width // width_gcd,
               height_num=height // height_gcd, height_den=reference_height // height_gcd,
               load=1, store=0)
    row.update(changes)
    return row


def counters(vertices=30, primitives=10, fragments=100):
    return dict(ia_vertices=vertices, ia_primitives=primitives, vs_invocations=vertices,
                clipping_invocations=primitives, clipping_primitives=primitives, fs_invocations=fragments)


def pass_row(**changes):
    row: dict = dict(schema=3, type='pass', window_start=1, window_end=300, key='a', kind='render', ordinal=1,
               gpu_ms=10.0, count=2, draws=10, stats_valid=2, samples=1, scene_relative=False,
               render_width=1280, render_height=720, reference_width=1280, reference_height=720,
               shader='shader-a', stats=counters())
    row.update(changes)
    if row['kind'] != 'render' and 'render_width' not in changes and 'render_height' not in changes:
        row.update(render_width=0, render_height=0)
    if 'attachments' not in changes:
        row['attachments'] = [attachment(width=row['render_width'], height=row['render_height'],
                                         reference_width=row['reference_width'],
                                         reference_height=row['reference_height'])]
    return row


def scene_row(width, height, reference_width=None, reference_height=None, depth_only=False, **changes):
    reference_width = width if reference_width is None else reference_width
    reference_height = height if reference_height is None else reference_height
    attachments = [attachment(width, height, reference_width, reference_height, slot=slot,
                              format=126 if slot == 8 else 37)
                   for slot in ((8,) if depth_only else (0, 1, 8))]
    return pass_row(render_width=width, render_height=height, reference_width=reference_width,
                    reference_height=reference_height, scene_relative=True, attachments=attachments, **changes)


def window(**changes):
    row = dict(schema=3, type='window', window_start=1, window_end=300, frames=300, partial=False,
               span_ms=12.0, measured_ms=10.0, gap_ms=2.0, dropped=0, failed_slots=0, stats_failed_slots=0)
    row.update(changes)
    return row


def frame(**changes):
    row = dict(schema=3, type='frame', frame=1, span_ms=12.0, measured_ms=10.0, gap_ms=2.0,
               dropped=0, reconciliation_failed=False, scene_width=0, scene_height=0)
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
                pass_row(key='c', attachments=[attachment(width=2560)], gpu_ms=3),
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
        a, b = scene_row(100000, 720), scene_row(100000, 720, key='b')
        a['attachments'][0] = attachment(100001, 720, 100000, 720)
        b['attachments'][0] = attachment(100002, 720, 100000, 720)
        report = self.analyze([a, b])
        self.assertEqual(len(report['families']), 2)
        self.assertEqual(report['coverage']['missing_window_summaries'], 1)

    def test_depth_only_absolute_sizes_are_separate_families(self):
        rows = [pass_row(key=str(size), render_width=size, render_height=size,
                         attachments=[attachment(size, size, slot=8, format=126)])
                for size in (360, 512)]
        report = self.analyze(rows)
        self.assertEqual(report['coverage']['malformed_rows'], 0)
        self.assertEqual(len(report['passes']), 2)
        self.assertEqual(len(report['families']), 2)
        self.assertEqual({(g['render_width'], g['render_height']) for g in report['passes']},
                         {(360, 360), (512, 512)})
        for group in report['families']:
            self.assertFalse(group['scene_relative'])
            self.assertEqual(len(group['areas']), 1)
            area = group['areas'][0]
            self.assertEqual(area['keys'], group['keys'])
            self.assertEqual(area['attachments'][0]['width'], area['render_width'])

    def test_changing_scene_merges_geometry_but_keeps_fixed_shadow_absolute(self):
        rows = []
        for index, (width, height) in enumerate(((1280, 720), (1600, 900)), 1):
            rows.append(scene_row(width, height, key=f'geometry-{width}', count=1, stats_valid=1))
            rows.append(pass_row(key='shadow', render_width=360, render_height=360,
                                 reference_width=width, reference_height=height, count=1, stats_valid=1,
                                 attachments=[attachment(360, 360, width, height, slot=8, format=126)]))
            rows.append(frame(frame=index, scene_width=width, scene_height=height))
        report = self.analyze([window(measured_ms=40, span_ms=42), *rows])
        self.assertEqual(report['schema'], 'totk-gpuprof-analysis/3')
        self.assertEqual(report['coverage']['conflicting_rows'], 0)
        self.assertEqual(report['coverage']['window_sum_mismatches'], 0)
        self.assertEqual(len(report['passes']), 3)
        self.assertEqual(len(report['families']), 2)
        geometry = next(g for g in report['families'] if g['scene_relative'])
        self.assertEqual(geometry['keys'], ['geometry-1280', 'geometry-1600'])
        self.assertEqual({(a['render_width'], a['render_height']) for a in geometry['areas']},
                         {(1280, 720), (1600, 900)})
        self.assertEqual({tuple(a['keys']) for a in geometry['areas']},
                         {('geometry-1280',), ('geometry-1600',)})
        shadow = next(g for g in report['families'] if not g['scene_relative'])
        self.assertEqual(shadow['keys'], ['shadow'])
        self.assertEqual(shadow['count'], 2)
        self.assertEqual({(a['render_width'], a['render_height']) for a in shadow['areas']}, {(360, 360)})
        self.assertEqual({(a['reference_width'], a['reference_height']) for a in shadow['areas']},
                         {(1280, 720), (1600, 900)})
        exact = next(g for g in report['passes'] if g['key'] == 'shadow')
        self.assertEqual((exact['render_width'], exact['render_height']), (360, 360))
        self.assertEqual((exact['attachments'][0]['width'], exact['attachments'][0]['height']), (360, 360))
        self.assertEqual(len(exact['areas']), 2)

    def test_scene_sized_depth_prepasses_merge_across_extents(self):
        report = self.analyze([scene_row(64, 36, depth_only=True, key='prepass-64'),
                               scene_row(128, 72, depth_only=True, key='prepass-128')])
        self.assertEqual(len(report['passes']), 2)
        self.assertEqual(len(report['families']), 1)
        group = report['families'][0]
        self.assertTrue(group['scene_relative'])
        self.assertEqual(group['keys'], ['prepass-128', 'prepass-64'])
        self.assertEqual({(a['render_width'], a['render_height']) for a in group['areas']},
                         {(64, 36), (128, 72)})

    def test_reference_variants_keep_one_exact_key_but_split_full_and_half_scene_families(self):
        for kind in ('pass', 'long_frame'):
            with self.subTest(kind=kind):
                rows = [scene_row(64, 36, key='geometry-64', type=kind, frame=1),
                        scene_row(128, 72, key='geometry-128', type=kind, frame=1),
                        scene_row(64, 36, 128, 72, key='geometry-64', type=kind, frame=1)]
                report = self.analyze(rows)
                prefix = 'long_frame_' if kind == 'long_frame' else ''
                self.assertEqual(report['coverage']['conflicting_rows'], 0)
                self.assertEqual(report['coverage']['duplicate_rows'], 0)
                self.assertEqual(len(report[prefix + 'passes']), 2)
                self.assertEqual(len(report[prefix + 'families']), 2)
                exact = next(g for g in report[prefix + 'passes'] if g['key'] == 'geometry-64')
                self.assertEqual(exact['gpu_ms'], 20)
                self.assertEqual(len(exact['areas']), 2)
                self.assertEqual({(a['reference_width'], a['reference_height']) for a in exact['areas']},
                                 {(64, 36), (128, 72)})
                full = next(g for g in report[prefix + 'families'] if len(g['keys']) == 2)
                self.assertEqual(full['gpu_ms'], 20)
                half = next(g for g in report[prefix + 'families'] if len(g['keys']) == 1)
                self.assertEqual(half['areas'][0]['attachments'][0]['width_num'], 1)
                self.assertEqual(half['areas'][0]['attachments'][0]['width_den'], 2)

    def test_scene_relative_flag_is_an_observation_and_family_identity(self):
        relative = scene_row(1280, 720, depth_only=True)
        absolute = {**relative, 'scene_relative': False}
        report = self.analyze([relative, absolute])
        self.assertEqual(report['coverage']['conflicting_rows'], 0)
        self.assertEqual(report['coverage']['pass_rows'], 2)
        self.assertEqual(len(report['passes']), 1)
        self.assertEqual(len(report['families']), 2)
        self.assertEqual({a['scene_relative'] for a in report['passes'][0]['areas']}, {False, True})

    def test_absolute_render_area_is_not_replaced_by_attachment_area(self):
        rows = [pass_row(key='same-driver-key', render_width=width, render_height=height,
                         attachments=[attachment(), attachment(slot=1), attachment(slot=8, format=126)])
                for width, height in ((640, 360), (1280, 720))]
        rows[1].update(window_start=301, window_end=600)
        report = self.analyze(rows)
        self.assertEqual(len(report['passes']), 2, 'different absolute areas must not disappear even for a reused key')
        self.assertEqual(len(report['families']), 2)
        self.assertEqual({g['attachments'][0]['width'] for g in report['passes']}, {1280})
        relative = [dict(row, scene_relative=True) for row in rows]
        self.assertEqual(len(self.analyze(relative)['families']), 2, 'render/reference crop ratio is structural')

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
        path = self.file('gpuprof-run.jsonl', [window(), pass_row()], '\nnot json\n{"schema":3')
        report = gpuprof.analyze([path])
        self.assertEqual(report['coverage']['malformed_rows'], 1)
        self.assertEqual(report['coverage']['truncated_rows'], 1)
        self.assertEqual(report['coverage']['blank_lines'], 1)
        self.assertEqual(report['coverage']['pass_rows'], 1)

    def test_invalid_schema_fields_are_rejected_without_losing_valid_rows(self):
        invalid = []
        for changes in ({'schema': 1}, {'schema': 2}, {'gpu_ms': -1},
                        {'gpu_ms': float('nan')}, {'gpu_ms': float('inf')},
                        {'draws': True}, {'stats_valid': 3}, {'stats': None}, {'type': 'other'},
                        {'attachments': [{**attachment(), 'width_den': 0}]},
                        {'attachments': [{**attachment(), 'width_num': 2, 'width_den': 2}]},
                        {'attachments': [attachment(), attachment()]}, {'ratios': {'ns_per_vertex': -1}}):
            invalid.append(pass_row(**changes))
        report = self.analyze([*invalid, pass_row()])
        self.assertEqual(report['coverage']['malformed_rows'], len(invalid))
        self.assertEqual(report['coverage']['pass_rows'], 1)

    def test_schema_two_is_rejected_for_every_row_type(self):
        rows = [window(), pass_row(), frame(), pass_row(type='long_frame', frame=1)]
        report = self.analyze([dict(row, schema=2) for row in rows])
        self.assertEqual(report['coverage']['malformed_rows'], 4)
        self.assertEqual(report['coverage']['pass_rows'], 0)
        self.assertEqual(report['coverage']['frame_rows'], 0)
        self.assertEqual(report['coverage']['windows'], 0)
        self.assertEqual(report['coverage']['long_frame_rows'], 0)

    def test_absolute_reference_and_scene_relative_fields_are_required(self):
        for kind in ('pass', 'long_frame'):
            for field in ('render_width', 'render_height', *gpuprof.REFERENCE):
                row = pass_row(type=kind, frame=1)
                del row[field]
                with self.subTest(kind=kind, field=field), self.assertRaises(ValueError):
                    gpuprof.validate(row)
        for field in ('width', 'height'):
            row = pass_row()
            del row['attachments'][0][field]
            with self.subTest(field=field), self.assertRaises(ValueError):
                gpuprof.validate(row)

    def test_dimensions_enforce_uint32_bounds_and_zero_pairing(self):
        for field in ('render_width', 'render_height', 'reference_width', 'reference_height'):
            for value in (0, -1, True, 1.5, gpuprof.UINT32_MAX + 1):
                row = pass_row()
                row[field] = value
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    gpuprof.validate(row)
        for field in ('width', 'height'):
            for value in (0, -1, True, 1.5, gpuprof.UINT32_MAX + 1):
                row = pass_row()
                row['attachments'][0][field] = value
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    gpuprof.validate(row)
        for value in (0, 1, 'true', None):
            with self.subTest(scene_relative=value), self.assertRaises(ValueError):
                gpuprof.validate(pass_row(scene_relative=value))
        self.assertEqual(gpuprof.validate(pass_row(kind='compute', attachments=[], samples=0))['render_width'], 0)
        for width, height in ((0, 1), (1, 0)):
            with self.subTest(width=width, height=height), self.assertRaises(ValueError):
                gpuprof.validate(pass_row(kind='compute', render_width=width, render_height=height, attachments=[]))

    def test_reference_bounds_are_not_limited_to_fallback_game_configuration(self):
        for width, height in ((1, 1), (4097, 16384), (gpuprof.UINT32_MAX, gpuprof.UINT32_MAX)):
            row = pass_row(render_width=width, render_height=height,
                           reference_width=width, reference_height=height)
            with self.subTest(width=width, height=height):
                self.assertIs(gpuprof.validate(row), row)
        row = pass_row(reference_width=1, reference_height=1)
        self.assertGreater(row['attachments'][0]['width_num'], 1, 'attachments may exceed the scene reference')
        self.assertIs(gpuprof.validate(row), row)

    def test_attachment_rationals_must_match_absolute_reference_dimensions(self):
        for axis in ('width', 'height'):
            for field, value in ((axis, 1), (f'{axis}_num', 0), (f'{axis}_den', 2)):
                row = pass_row()
                row['attachments'][0][field] = value
                with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'rational'):
                    gpuprof.validate(row)
        row = pass_row(render_width=360, render_height=360,
                       attachments=[attachment(360, 360, slot=8, format=126)])
        self.assertEqual((row['attachments'][0]['width_num'], row['attachments'][0]['width_den']), (9, 32))
        self.assertEqual((row['attachments'][0]['height_num'], row['attachments'][0]['height_den']), (1, 2))
        self.assertIs(gpuprof.validate(row), row, 'ratios are against the common reference, not pass area')

    def test_frame_scene_dimensions_are_required_paired_and_preserved(self):
        rows = [frame(), frame(frame=2, scene_width=1280, scene_height=720),
                frame(frame=3, scene_width=gpuprof.UINT32_MAX, scene_height=gpuprof.UINT32_MAX)]
        path = self.file('gpuprof-scenes.jsonl', rows)
        observations, counts, _ = gpuprof.load([path])
        self.assertEqual(counts['malformed_rows'], 0)
        self.assertEqual([o['row'] for o in observations], rows)
        for field in ('scene_width', 'scene_height'):
            for value in (None, -1, True, 1.5, gpuprof.UINT32_MAX + 1):
                row = frame(scene_width=1280, scene_height=720)
                if value is None:
                    del row[field]
                else:
                    row[field] = value
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    gpuprof.validate(row)
        for width, height in ((0, 720), (1280, 0)):
            with self.subTest(width=width, height=height), self.assertRaises(ValueError):
                gpuprof.validate(frame(scene_width=width, scene_height=height))

    def test_rotated_reference_variants_are_not_duplicate_or_conflicting_rows(self):
        a = scene_row(64, 36)
        b = scene_row(64, 36, 128, 72)
        self.file('gpuprof-refs.jsonl.1', [a])
        self.file('gpuprof-refs.jsonl', [a, b])
        report = gpuprof.analyze([str(self.root / '*.jsonl*')])
        self.assertEqual(report['coverage']['duplicate_rows'], 1)
        self.assertEqual(report['coverage']['conflicting_rows'], 0)
        self.assertEqual(report['coverage']['pass_rows'], 2)
        self.assertEqual(len(report['passes'][0]['areas']), 2)

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
        self.assertEqual(json.loads(path.read_text().splitlines()[0])['schema'], 3)

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

    def test_render_includes_absolute_join_fields_and_all_reference_variants(self):
        rows = [scene_row(64, 36), scene_row(64, 36, 128, 72)]
        report = self.analyze(rows)
        group = report['passes'][0]
        self.assertEqual((group['render_width'], group['render_height']), (64, 36))
        self.assertEqual({(a['width'], a['height']) for a in group['attachments']}, {(64, 36)})
        text = io.StringIO()
        gpuprof.render(report, text)
        rendered = text.getvalue()
        self.assertIn('schema 3', rendered)
        self.assertIn('render area(s)', rendered)
        self.assertIn('64x36', rendered)
        self.assertIn('"width":64', rendered)
        self.assertIn('"height":36', rendered)
        self.assertIn('"reference_width":64', rendered)
        self.assertIn('"reference_width":128', rendered)


@unittest.skipUnless(os.environ.get('GPUPROF_TEST_BIN'), 'set GPUPROF_TEST_BIN to run the real Vulkan fixture')
class Lavapipe(unittest.TestCase):
    def launch(self, mode, workers, stats, *, unsupported=False, rotate=False, areas=False):
        binary = Path(os.environ['GPUPROF_TEST_BIN']).resolve()
        self.assertTrue(binary.is_file(), f'no fixture binary: {binary}')
        seed = Path(os.environ.get('TOTK_SHADER_SEED_DIR', binary.parent / 'vkdraw-seed')).resolve()
        self.assertTrue(seed.is_dir(), f'no shader seed: {seed}')
        with tempfile.TemporaryDirectory(prefix='gpuprof-test-') as temporary:
            env = dict(os.environ, TOTK_SHADER_SEED_DIR=str(seed), TOTK_VK_GPU_PROFILE=str(mode),
                       TOTK_VK_REC_THREADS=str(workers), TOTK_VK_GPU_PROFILE_STATS=str(stats),
                       TOTK_VK_GPU_PROFILE_LONG_MS='0', TOTK_VK_GPU_PROFILE_LONG_FRAMES='2',
                       TOTK_VK_GPU_PROFILE_MAX_BYTES='4096' if rotate else '16777216',
                       TOTK_RENDER_RES='1280x720')
            # Isolate launch policy from a developer's shell diagnostics.
            for key in ('TOTK_VK_OCCLUSION', 'TOTK_VK_PASS_DUMP', 'TOTK_VK_PASS_PROFILE', 'TOTK_VK_PASSPROBE',
                        'TOTK_VK_PASS_MERGE', 'GPUPROF_TEST_UNSUPPORTED', 'TOTK_VK_OFF', 'TOTK_VK_WRITEBACK',
                        'TOTK_FRAME_LAG', 'TOTK_VK_LAG_CAPTURE'):
                env.pop(key, None)
            if unsupported:
                env['GPUPROF_TEST_UNSUPPORTED'] = '1'
            arguments = ['--areas'] if areas else ['--frames', '302']
            result = subprocess.run([str(binary), *arguments], cwd=temporary, env=env,
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
            for observation in observations:
                row = observation['row']
                self.assertEqual(row['schema'], 3)
                if row['type'] in ('pass', 'long_frame'):
                    for axis in ('width', 'height'):
                        self.assertGreater(row[f'reference_{axis}'], 0)
                        self.assertLessEqual(row[f'reference_{axis}'], gpuprof.UINT32_MAX)
                        self.assertEqual(row[f'render_{axis}'] > 0, row['kind'] == 'render')
                        for attachment in row['attachments']:
                            self.assertEqual(attachment[f'{axis}_num'] * row[f'reference_{axis}'],
                                             attachment[axis] * attachment[f'{axis}_den'])
                    self.assertIsInstance(row['scene_relative'], bool)
            if areas:
                self.assertEqual((frames, passes, draws), (3, 11, 0))
                self.assertEqual(bool(enabled), bool(stats and supported and not unsupported))
                self.check_areas(report, observations, result.stdout, bool(enabled))
                return report
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
            self.assertEqual(len({g['key'] for g in render_groups}), 2,
                             'absolute 64/128 extents must have different exact render keys')
            self.assertEqual({(g['render_width'], g['render_height']) for g in render_groups},
                             {(64, 64), (128, 128)})
            render_families = [g for g in report['families'] if g['kind'] == 'render']
            self.assertEqual(len(render_families), 2, 'color-only scopes retain absolute family dimensions')
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
                    self.assertEqual((row['reference_width'], row['reference_height']), (1280, 720))
                    self.assertFalse(row['scene_relative'])
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
                    self.assertEqual((row['scene_width'], row['scene_height']), (0, 0))
            missing = sum(g['count'] for g in other_groups) + (0 if enabled else passes)
            self.assertEqual(report['coverage']['stats_missing'], missing)
            if mode == 3:
                self.assertEqual(report['coverage']['long_frames'], frames,
                                 'threshold zero must dump every frame, independent of history ring retention')
            else:
                self.assertEqual(report['coverage']['long_frames'], 0)
            return report

    def check_areas(self, report, observations, stdout, stats_enabled):
        self.assertIn('gpuprof-areas frames=3 passes=11 draws=0 exact_keys=6 families=5 transfers=17', stdout)
        metadata = re.findall(
            r'gpuprof-areas-frame frame=(\d+) scene_width=(\d+) scene_height=(\d+) passes=(\d+) '
            r'prepass=(\d+) geometry=(\d+) shadow=1 other_depth=1 transfers=(\d+)', stdout)
        self.assertEqual([tuple(map(int, row)) for row in metadata],
                         [(1, 64, 36, 4, 1, 1, 6), (2, 128, 72, 5, 1, 2, 9), (3, 0, 0, 2, 0, 0, 2)])
        for key in ('window_sum_mismatches', 'missing_frame_ids', 'reconciliation_failures', 'dropped',
                    'failed_slots', 'long_frame_rows'):
            self.assertEqual(report['coverage'][key], 0, report['issues'])
        self.assertEqual(report['coverage']['frame_rows'], 3)
        self.assertEqual(report['coverage']['windows'], 1)
        self.assertAlmostEqual(report['timing']['window']['measured_ms'],
                               report['timing']['frame']['measured_ms'], delta=0.01)
        groups = [g for g in report['passes'] if g['kind'] == 'render']
        families = [g for g in report['families'] if g['kind'] == 'render']
        self.assertEqual(sum(g['count'] for g in groups), 11)
        self.assertEqual(sum(g['draws'] for g in groups), 0)
        self.assertEqual(len({g['key'] for g in groups}), 6)
        self.assertEqual(len(families), 5)
        self.assertEqual(sum(g['count'] for g in report['passes'] if g['kind'] == 'transfer'), 17)
        relative = [g for g in families if g['scene_relative']]
        self.assertEqual(len(relative), 3)
        geometry = [g for g in relative if len(g['areas'][0]['attachments']) == 3]
        prepass = [g for g in relative if len(g['areas'][0]['attachments']) == 1]
        self.assertEqual(len(geometry), 2)
        self.assertEqual(len(prepass), 1)
        self.assertEqual(prepass[0]['count'], 2)
        self.assertEqual(len(prepass[0]['keys']), 2)
        self.assertEqual({(a['render_width'], a['render_height']) for a in prepass[0]['areas']},
                         {(64, 36), (128, 72)})
        full = next(g for g in geometry if len(g['keys']) == 2)
        half = next(g for g in geometry if len(g['keys']) == 1)
        self.assertEqual(full['count'], 2)
        self.assertEqual(half['count'], 1)
        self.assertEqual({(a['render_width'], a['render_height']) for a in full['areas']}, {(64, 36), (128, 72)})
        self.assertEqual((half['areas'][0]['render_width'], half['areas'][0]['render_height']), (64, 36))
        for attachment in half['areas'][0]['attachments']:
            self.assertEqual((attachment['width_num'], attachment['width_den']), (1, 2))
            self.assertEqual((attachment['height_num'], attachment['height_den']), (1, 2))
        smaller_key = half['keys'][0]
        smaller_areas = [a for g in groups if g['key'] == smaller_key for a in g['areas']]
        self.assertEqual({(a['reference_width'], a['reference_height']) for a in smaller_areas},
                         {(64, 36), (128, 72)})
        fixed = [g for g in families if not g['scene_relative']]
        self.assertEqual(len(fixed), 2)
        self.assertEqual({a['render_width'] for g in fixed for a in g['areas']}, {128, 360})
        for group in fixed:
            self.assertEqual(group['count'], 3)
            self.assertEqual(len(group['keys']), 1, 'fixed depth key must not follow changing scene references')
            self.assertEqual({(a['reference_width'], a['reference_height']) for a in group['areas']},
                             {(64, 36), (128, 72), (1280, 720)})
            for area in group['areas']:
                self.assertEqual(area['keys'], group['keys'])
                self.assertEqual(len(area['attachments']), 1)
                attachment = area['attachments'][0]
                self.assertEqual(attachment['slot'], 8)
                self.assertEqual((attachment['width'], attachment['height']),
                                 (area['render_width'], area['render_height']))
        expected_scenes = {1: (64, 36), 2: (128, 72), 3: (0, 0)}
        frame_rows = [o['row'] for o in observations if o['row']['type'] == 'frame']
        self.assertEqual({r['frame']: (r['scene_width'], r['scene_height']) for r in frame_rows}, expected_scenes)
        for row in frame_rows:
            self.assertFalse(row['reconciliation_failed'])
            self.assertEqual(row['dropped'], 0)
        for observation in observations:
            row = observation['row']
            if row['type'] != 'pass':
                continue
            self.assertEqual('stats' in row, bool(row['stats_valid']))
            self.assertLessEqual(row['stats_valid'], row['count'])
            if not stats_enabled or row['kind'] != 'render':
                self.assertEqual(row['stats_valid'], 0)
            if row['stats_valid']:
                self.assertTrue(all(row['stats'][key] == 0 for key in gpuprof.STATS))
        self.assertEqual(report['coverage']['stats_missing'],
                         sum(o['row']['count'] - o['row']['stats_valid']
                             for o in observations if o['row']['type'] == 'pass'))

    def test_scene_areas_prepasses_and_fixed_depth_families(self):
        reference = None
        for mode in (1, 2):
            for workers in (0, 2):
                for stats in (0, 1):
                    with self.subTest(mode=mode, workers=workers, stats=stats):
                        report = self.launch(mode, workers, stats, areas=True)
                        keys = {(g['key'], g['render_width'], g['render_height'])
                                for g in report['passes'] if g['kind'] == 'render'}
                        if reference is None:
                            reference = keys
                        self.assertEqual(keys, reference)

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
