"""Synthetic saved-log fixtures; no game, GPU, device, compiler or native pixel claims."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    'target_lifetime', ROOT / 'games/totk/android/analyze_target_lifetime.py')
assert SPEC is not None and SPEC.loader is not None
lifetime = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(lifetime)
SCRATCH = ROOT / '.scratch/screen-classify'


def row(kind, **fields):
    return '[pso-life] ' + ' '.join(f'{key}={value}' for key, value in dict(kind=kind, **fields).items())


def image_row(kind, frame, **changes):
    fields = dict(frame=frame, sub=frame, image='0x10', incarnation=1,
                  mip=0, layer=0, aspect=1, w=480, h=268, format=122,
                  serial=1, storage='0x100')
    fields['pass'] = frame
    if kind == 'write':
        fields.update(cleared=1, draws=1)
    else:
        fields.update(stage=4, binding=2, access='sampled', shader='sky-key',
                      consumer_w=960, consumer_h=536)
    fields.update(changes)
    return row(kind, **fields)


def trace(frame_events, *, start=1, limit=None, close=True, include_start=False):
    """Emit frame-end row counts, including storage-write descriptor rows."""
    limit = len(frame_events) if limit is None else limit
    begin = dict(w=480, h=268, limit=limit)
    if start != 1 or include_start:
        begin['start'] = start
    lines = [row('begin', **begin)]
    unknown_total = 0
    for frame, events in enumerate(frame_events, start):
        lines.extend(events)
        unknown = sum('kind=unknown ' in event for event in events)
        unknown_total += unknown
        lines.append(row('frame-end', frame=frame,
                         reads=sum('kind=read ' in event for event in events),
                         writes=sum('kind=write ' in event for event in events),
                         unknown=unknown))
    if close:
        lines.append(row('end', frame=start + limit - 1, complete=1, unknown=unknown_total))
    return '\n'.join(lines) + '\n'


def overwritten(frame, **fields):
    return [image_row('write', frame, **fields), image_row('read', frame, **fields)]


class TargetLifetimeTests(unittest.TestCase):
    def test_current_frame_overwrite_then_sample_is_bounded_candidate(self):
        text = 'unrelated startup\n' + trace([overwritten(1), overwritten(2), overwritten(3)])
        report = lifetime.analyze(text)
        self.assertEqual(report['verdict'], 'transient_candidate')
        self.assertTrue(report['trace']['complete'])
        self.assertEqual(report['summary']['established_prior_frame_reads'], 0)
        self.assertEqual(report['summary']['transient_candidate_blockers'], [])
        self.assertEqual(report['scope']['transient_candidate_first_frame'], 2)
        self.assertEqual(report['identities'][0]['write_frames'], [1, 2, 3])
        read = report['frames'][1]['reads'][0]
        self.assertEqual(read['classification'], 'current_frame_producer')
        self.assertEqual(read['previous_writer_frame'], 2)
        self.assertEqual(read['age_frames'], 0)
        self.assertFalse(read['read_before_current_write'])
        self.assertTrue(read['base_established_current_frame'])
        self.assertEqual(text.splitlines()[read['line'] - 1], read['raw'])
        self.assertEqual(text.splitlines()[read['previous_writer_line'] - 1],
                         report['frames'][1]['writes'][0]['raw'])
        self.assertTrue(any('MAY-read' in value for value in report['limitations']))
        self.assertTrue(any('packed-pixel' in value for value in report['limitations']))

    def test_alternating_double_buffers_establish_history_not_transience(self):
        report = lifetime.analyze(trace([
            overwritten(1),
            [image_row('read', 2), *overwritten(2, image='0x20')],
            [image_row('read', 3, image='0x20'), *overwritten(3)],
        ]))
        self.assertEqual(report['verdict'], 'history_or_persistent')
        self.assertEqual(report['summary']['root_count'], 2)
        self.assertEqual(report['groups'][0]['root_count'], 2)
        self.assertTrue(report['groups'][0]['alternating_two_root_writes'])
        self.assertFalse(report['groups'][0]['recurrence_is_lifetime_proof'])
        self.assertEqual([(read['frame'], read['previous_writer_frame'], read['age_frames'])
                          for read in report['established_prior_frame_reads']],
                         [(2, 1, 1), (3, 2, 1)])
        identity = next(value for value in report['identities'] if value['image'] == '0x10')
        self.assertEqual(identity['write_gaps'][0]['gap_frames'], 1)
        self.assertEqual(identity['write_gaps'][0]['elapsed_frames'], 2)

    def test_alternating_buffers_without_old_read_do_not_prove_history(self):
        report = lifetime.analyze(trace([overwritten(1), overwritten(2, image='0x20'),
                                         overwritten(3)]))
        self.assertEqual(report['verdict'], 'transient_candidate')
        self.assertTrue(report['groups'][0]['alternating_two_root_writes'])
        self.assertEqual(report['established_prior_frame_reads'], [])

    def test_same_address_new_incarnation_is_unknown_not_false_history(self):
        report = lifetime.analyze(trace([
            overwritten(1), [image_row('read', 2, incarnation=2)],
        ]))
        self.assertEqual(report['verdict'], 'unknown')
        self.assertEqual(report['summary']['identity_count'], 2)
        self.assertEqual(report['summary']['unknown_initial_reads'], 1)
        self.assertEqual(report['summary']['established_prior_frame_reads'], 0)
        self.assertIsNone(report['unknown_initial_reads'][0]['previous_writer_frame'])
        self.assertEqual(report['image_address_reuse'], [dict(image='0x10', incarnations=[1, 2])])

    def test_same_incarnation_new_serial_and_storage_still_detect_history(self):
        report = lifetime.analyze(trace([
            overwritten(1), [image_row('read', 2, serial=77, storage='0x900')],
        ]))
        self.assertEqual(report['verdict'], 'history_or_persistent')
        self.assertEqual(report['summary']['identity_count'], 1)
        read = report['established_prior_frame_reads'][0]
        self.assertEqual((read['previous_writer_frame'], read['age_frames']), (1, 1))
        identity = report['identities'][0]
        self.assertEqual(identity['serials'], [1, 77])
        self.assertEqual(identity['storage_addresses'], ['0x100', '0x900'])
        self.assertEqual(identity['serial_changes'][0]['previous'], 1)
        self.assertEqual(identity['serial_changes'][0]['value'], 77)
        self.assertEqual(identity['serial_changes'][0]['line'], read['line'])
        self.assertEqual(identity['storage_changes'][0]['value'], '0x900')

    def test_each_subresource_component_is_identity(self):
        for changed in (dict(mip=1), dict(layer=1), dict(aspect=2), dict(image='0x11')):
            with self.subTest(changed=changed):
                report = lifetime.analyze(trace([overwritten(1), [image_row('read', 2, **changed)]]))
                self.assertEqual(report['verdict'], 'unknown')
                self.assertEqual(report['summary']['identity_count'], 2)
                self.assertEqual(report['unknown_initial_reads'][0]['classification'], 'unknown_initial_read')

    def test_first_window_boundary_unknown_is_not_established_history(self):
        report = lifetime.analyze(trace([
            [image_row('read', 1), *overwritten(1)], overwritten(2),
        ]))
        self.assertEqual(report['verdict'], 'transient_candidate')
        self.assertEqual(report['summary']['first_frame_boundary_unknown_reads'], 1)
        self.assertEqual(report['summary']['unknown_initial_reads'], 0)
        self.assertEqual(report['summary']['established_prior_frame_reads'], 0)
        read = report['first_frame_boundary_unknown_reads'][0]
        self.assertIsNone(read['previous_writer_frame'])
        self.assertIsNone(read['age_frames'])
        self.assertTrue(read['read_before_current_write'])

    def test_delayed_window_limit_is_length_and_boundary_is_relative(self):
        report = lifetime.analyze(trace([
            [image_row('read', 3000), *overwritten(3000)], overwritten(3001),
        ], start=3000))
        self.assertEqual(report['verdict'], 'transient_candidate')
        self.assertEqual(report['scope']['first_recording_frame'], 3000)
        self.assertEqual(report['scope']['requested_last_frame'], 3001)
        self.assertEqual(report['scope']['requested_frames'], 2)
        self.assertEqual(report['scope']['transient_candidate_first_frame'], 3001)
        self.assertEqual(report['trace']['missing_frame_end_ranges'], [])
        self.assertEqual(report['first_frame_boundary_unknown_reads'][0]['frame'], 3000)
        history = lifetime.analyze(trace([
            overwritten(3000), [image_row('read', 3001)],
        ], start=3000))
        self.assertEqual(history['verdict'], 'history_or_persistent')
        self.assertEqual(history['established_prior_frame_reads'][0]['age_frames'], 1)

    def test_missing_frame_end_or_end_cannot_prove_transience(self):
        good = trace([overwritten(1), overwritten(2)])
        cases = [good.replace(row('frame-end', frame=1, reads=1, writes=1, unknown=0) + '\n', ''),
                 good.rsplit('[pso-life] kind=end', 1)[0]]
        for text in cases:
            with self.subTest(text=text):
                report = lifetime.analyze(text)
                self.assertEqual(report['verdict'], 'unknown')
                self.assertFalse(report['trace']['complete'])
        self.assertEqual(lifetime.analyze(cases[0])['trace']['missing_frame_end_ranges'],
                         [dict(first=1, last=1)])

    def test_gap_in_writer_coverage_is_not_established_history(self):
        text = '\n'.join([
            row('begin', w=480, h=268, limit=3),
            *overwritten(1), row('frame-end', frame=1, reads=1, writes=1, unknown=0),
            image_row('read', 3), row('frame-end', frame=3, reads=1, writes=0, unknown=0),
            row('end', frame=3, complete=1, unknown=0),
        ])
        report = lifetime.analyze(text)
        self.assertEqual(report['verdict'], 'unknown')
        read = report['frames'][1]['reads'][0]
        self.assertEqual(read['previous_writer_frame'], 1)
        self.assertEqual(read['age_frames'], 2)
        self.assertEqual(read['classification'], 'prior_writer_unverified_coverage')
        self.assertFalse(read['established_prior_frame'])

    def test_established_history_survives_a_later_missing_end(self):
        report = lifetime.analyze(trace([overwritten(1), [image_row('read', 2)]], close=False))
        self.assertEqual(report['verdict'], 'history_or_persistent')
        self.assertFalse(report['trace']['complete'])
        self.assertEqual(report['established_prior_frame_reads'][0]['previous_writer_frame'], 1)

    def test_exact_age_uses_last_writer_not_previous_present(self):
        report = lifetime.analyze(trace([
            overwritten(1), [], [image_row('read', 3)],
        ]))
        self.assertEqual(report['verdict'], 'history_or_persistent')
        read = report['established_prior_frame_reads'][0]
        self.assertEqual((read['previous_writer_frame'], read['age_frames']), (1, 2))

    def test_load_write_without_current_full_clear_has_preserved_base_caveat(self):
        report = lifetime.analyze(trace([overwritten(1), overwritten(2, cleared=0)]))
        self.assertEqual(report['verdict'], 'unknown')
        read = report['frames'][1]['reads'][0]
        self.assertEqual(read['classification'], 'current_frame_producer')
        self.assertTrue(read['load_preserved_base_caveat'])
        self.assertFalse(read['writer_full_clear'])
        self.assertEqual(read['base_frame'], 1)
        self.assertFalse(read['base_established_current_frame'])
        self.assertEqual(report['load_base_caveats'][0]['frame'], 2)

    def test_load_after_known_current_clear_retains_caveat_and_current_base(self):
        report = lifetime.analyze(trace([overwritten(1), [
            image_row('write', 2), image_row('write', 2, cleared=0), image_row('read', 2),
        ]]))
        self.assertEqual(report['verdict'], 'transient_candidate')
        self.assertTrue(report['frames'][1]['reads'][0]['load_preserved_base_caveat'])
        self.assertTrue(report['load_base_caveats'][0]['base_established_current_frame'])

    def test_old_read_from_load_writer_is_history_with_caveat(self):
        report = lifetime.analyze(trace([
            overwritten(1, cleared=0), [image_row('read', 2)],
        ]))
        self.assertEqual(report['verdict'], 'history_or_persistent')
        self.assertTrue(report['established_prior_frame_reads'][0]['load_preserved_base_caveat'])
        self.assertIsNone(report['established_prior_frame_reads'][0]['base_frame'])

    def test_non_render_partial_write_is_not_full_overwrite_authority(self):
        for source in ('upload', 'copy', 'clear'):
            with self.subTest(source=source):
                report = lifetime.analyze(trace([overwritten(1), [
                    image_row('write', 2, source=source, cleared=0, draws=0), image_row('read', 2),
                ]]))
                self.assertEqual(report['trace']['malformed_records'], 0)
                self.assertEqual(report['verdict'], 'unknown')
                self.assertEqual(report['frames'][1]['writes'][0]['source'], source)
                self.assertFalse(report['frames'][1]['reads'][0]['base_established_current_frame'])
        report = lifetime.analyze(trace([overwritten(1), [
            image_row('write', 2, source='clear', cleared=1, draws=0), image_row('read', 2),
        ]]))
        self.assertEqual(report['verdict'], 'transient_candidate')

    def test_storage_write_descriptor_is_not_a_history_read_or_pass_write(self):
        report = lifetime.analyze(trace([overwritten(1), [
            image_row('read', 2, access='storage-write'), image_row('read', 2),
        ]]))
        self.assertEqual(report['verdict'], 'unknown')
        self.assertEqual(report['summary']['established_prior_frame_reads'], 0)
        bucket = report['frames'][1]
        self.assertTrue(bucket['coverage']['complete'])
        self.assertEqual(bucket['coverage']['observed_reads'], 2)
        self.assertEqual(bucket['writes'], [])
        self.assertEqual(bucket['reads'][0]['classification'], 'storage_write_descriptor')
        self.assertEqual(bucket['reads'][1]['classification'], 'unknown_intervening_storage_write')
        storage_read = lifetime.analyze(trace([
            overwritten(1), [image_row('read', 2, access='storage-read')],
        ]))
        self.assertEqual(storage_read['verdict'], 'history_or_persistent')

    def test_accounted_readonly_unknowns_preserve_history_writer_authority(self):
        reasons = (
            'texture-to-buffer-read', 'texture-copy-read', 'writeback-read',
            'native-image-read', 'diagnostic-readback', 'cpu-texel-read',
            'presenter-read', 'image-upgrade-read',
        )
        self.assertEqual(set(reasons), lifetime.READ_ONLY_UNKNOWN_REASONS)
        for reason in reasons:
            with self.subTest(reason=reason):
                report = lifetime.analyze(trace([
                    [*overwritten(1), row('unknown', frame=1, reason=reason)],
                    [image_row('read', 2), row('unknown', frame=2, reason=reason)],
                ]))
                self.assertEqual(report['verdict'], 'history_or_persistent')
                self.assertFalse(report['trace']['complete'])
                self.assertEqual(report['trace']['reported_frame_unknown_total'], 2)
                for bucket in report['frames']:
                    coverage = bucket['coverage']
                    self.assertFalse(coverage['complete'])
                    self.assertTrue(coverage['writer_complete'])
                    self.assertEqual(coverage['writer_reasons'], [])
                    self.assertTrue(coverage['unknown_rows_accounted'])
                    self.assertEqual(coverage['readonly_unknown_rows'], 1)
                read = report['established_prior_frame_reads'][0]
                self.assertEqual((read['previous_writer_frame'], read['age_frames']), (1, 1))
                self.assertTrue(read['writer_coverage_established'])

    def test_readonly_unknowns_still_block_transient_certification(self):
        report = lifetime.analyze(trace([
            [*overwritten(frame),
             row('unknown', frame=frame, reason='diagnostic-readback'),
             row('unknown', frame=frame, reason='texture-copy-read')]
            for frame in (1, 2)
        ]))
        self.assertEqual(report['verdict'], 'unknown')
        self.assertFalse(report['trace']['complete'])
        self.assertEqual(report['established_prior_frame_reads'], [])
        self.assertIn('incomplete_or_unknown_trace_authority',
                      report['summary']['transient_candidate_blockers'])
        self.assertTrue(all(bucket['coverage']['writer_complete'] for bucket in report['frames']))
        self.assertTrue(all(bucket['coverage']['readonly_unknown_rows'] == 2
                            for bucket in report['frames']))

    def test_generic_or_writer_unknown_invalidates_history_writer_coverage(self):
        for reason in (
            'capacity', 'unresolved_view', 'writer-view-identity',
            'external-write-inside-pass', 'write-layer-range', '3d-write-range',
            'texture-copy-read-extra',
        ):
            with self.subTest(reason=reason):
                report = lifetime.analyze(trace([
                    [*overwritten(1),
                     row('unknown', frame=1, reason='diagnostic-readback'),
                     row('unknown', frame=1, reason=reason)],
                    [image_row('read', 2)],
                ]))
                self.assertEqual(report['verdict'], 'unknown')
                coverage = report['frames'][0]['coverage']
                self.assertTrue(coverage['unknown_rows_accounted'])
                self.assertFalse(coverage['writer_complete'])
                self.assertIn('runtime_unknown', coverage['writer_reasons'])
                read = report['frames'][1]['reads'][0]
                self.assertEqual(read['previous_writer_frame'], 1)
                self.assertEqual(read['classification'], 'prior_writer_unverified_coverage')
                self.assertFalse(read['writer_coverage_established'])
                self.assertEqual(report['established_prior_frame_reads'], [])

    def test_readonly_unknown_rows_require_exact_frame_counter_accounting(self):
        good_counts = trace([
            [*overwritten(1), row('unknown', frame=1, reason='diagnostic-readback')],
            [image_row('read', 2)],
        ])
        for reported_count in (0, 2):
            with self.subTest(reported_count=reported_count):
                text = good_counts.replace(
                    'kind=frame-end frame=1 reads=1 writes=1 unknown=1',
                    f'kind=frame-end frame=1 reads=1 writes=1 unknown={reported_count}')
                report = lifetime.analyze(text)
                self.assertEqual(report['verdict'], 'unknown')
                coverage = report['frames'][0]['coverage']
                self.assertFalse(coverage['unknown_rows_accounted'])
                self.assertFalse(coverage['writer_complete'])
                self.assertIn('unknown_count_mismatch', coverage['writer_reasons'])
                self.assertEqual(report['established_prior_frame_reads'], [])

    def test_unknown_record_counter_capacity_and_truncation_fail_closed(self):
        for reason in ('capacity', 'truncated', 'unresolved_view'):
            with self.subTest(reason=reason):
                report = lifetime.analyze(trace([overwritten(1), [
                    *overwritten(2), row('unknown', frame=2, reason=reason),
                ]]))
                self.assertEqual(report['verdict'], 'unknown')
                self.assertFalse(report['trace']['complete'])
                self.assertEqual(report['trace']['unknown_records'][0]['reason'], reason)
                self.assertEqual(report['trace']['reported_frame_unknown_total'], 1)
        text = trace([overwritten(1), overwritten(2)]).replace(
            'kind=end frame=2 complete=1 unknown=0', 'kind=end frame=2 complete=1 unknown=1')
        report = lifetime.analyze(text)
        self.assertEqual(report['verdict'], 'unknown')
        self.assertTrue(any(issue['code'] == 'unlocalized_end_unknown'
                            for issue in report['trace']['issues']))

    def test_missing_row_count_mismatch_fails_closed(self):
        text = trace([overwritten(1), overwritten(2)]).replace(
            'kind=frame-end frame=2 reads=1 writes=1', 'kind=frame-end frame=2 reads=2 writes=1')
        report = lifetime.analyze(text)
        self.assertEqual(report['verdict'], 'unknown')
        self.assertIn('reads_count_mismatch', report['frames'][1]['coverage']['reasons'])

    def test_malformed_lines_are_preserved_and_do_not_crash(self):
        malformed = [
            '[pso-life] kind=read frame=2',
            '[pso-life] kind=begin w=480 h=268 limit=2 limit=3',
            '[pso-life] kind=mystery frame=2',
            image_row('read', 2).replace('incarnation=1', 'incarnation=0'),
            image_row('read', 2).replace('image=0x10', 'image=not-hex'),
            image_row('read', 2).replace('frame=2', 'frame=-1'),
            image_row('read', 2).replace('access=sampled', 'access=guess'),
            image_row('write', 2, cleared=0, draws=0),
            image_row('write', 2, cleared=2),
            '[pso-life] kind=unknown reason=',
        ]
        for bad in malformed:
            with self.subTest(bad=bad):
                text = trace([overwritten(1), overwritten(2)]) + bad + '\n'
                report = lifetime.analyze(text)
                self.assertEqual(report['verdict'], 'unknown')
                self.assertEqual(report['trace']['malformed_records'], 1)
                issue = next(item for item in report['trace']['issues']
                             if item['code'] == 'malformed_record')
                self.assertEqual(issue['raw'], bad)
                self.assertEqual(text.splitlines()[issue['line'] - 1], bad)

    def test_no_records_single_frame_wrong_extent_and_restarted_log_are_unknown(self):
        for text in ('ordinary log\n', trace([overwritten(1)]),
                     trace([overwritten(1), overwritten(2, w=240, h=134)]),
                     trace([overwritten(1), overwritten(2)]) * 2):
            with self.subTest(text=text):
                report = lifetime.analyze(text)
                self.assertEqual(report['verdict'], 'unknown')
        one = lifetime.analyze(trace([overwritten(1)]))
        self.assertTrue(one['trace']['complete'])
        self.assertIn('no_post_boundary_frame', one['summary']['transient_candidate_blockers'])

    def test_boundary_only_or_unread_identity_cannot_inherit_another_proof(self):
        for events in (
            [overwritten(1, image='0x20'), overwritten(2)],
            [overwritten(1), [*overwritten(2), image_row('write', 2, image='0x20')]],
        ):
            with self.subTest(events=events):
                report = lifetime.analyze(trace(events))
                self.assertEqual(report['verdict'], 'unknown')
                self.assertTrue(any('lacks_post_boundary_write_read_evidence' in blocker
                                    for blocker in report['summary']['transient_candidate_blockers']))

    def test_prefixed_lines_hex_normalization_and_json_are_deterministic(self):
        text = trace([overwritten(1, image='0x001A', storage='0X0F00'),
                      overwritten(2, image='0X1a', storage='0xf00')])
        text = '\n'.join('10-28 10:00:00.000 I renderer: ' + line for line in text.splitlines())
        first = lifetime.analyze(text)
        second = lifetime.analyze(text)
        self.assertEqual(json.dumps(first, sort_keys=True), json.dumps(second, sort_keys=True))
        self.assertEqual(first['summary']['identity_count'], 1)
        self.assertEqual(first['identities'][0]['image'], '0x1a')
        self.assertEqual(first['identities'][0]['storage_addresses'], ['0xf00'])

    def test_cli_stdout_optional_output_and_source_digest(self):
        SCRATCH.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='target-life-', dir=SCRATCH) as temporary:
            directory = Path(temporary)
            path, output = directory / 'capture.log', directory / 'report.json'
            contents = trace([overwritten(1), overwritten(2)]).encode('utf-8')
            path.write_bytes(contents)
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                self.assertEqual(lifetime.main([str(path)]), 0)
            self.assertEqual(lifetime.main([str(path), '--output', str(output)]), 0)
            self.assertEqual(stdout.getvalue(), output.read_text(encoding='utf-8'))
            report = json.loads(stdout.getvalue())
            self.assertEqual(report['source']['sha256'], hashlib.sha256(contents).hexdigest())
            self.assertEqual(report['source']['bytes'], len(contents))
            self.assertEqual(report['source']['path'], str(path.resolve()))
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(lifetime.main([str(path), '--output', str(path)]), 2)
                self.assertEqual(lifetime.main([str(directory / 'missing.log')]), 2)
            self.assertEqual(path.read_bytes(), contents)


if __name__ == '__main__':
    unittest.main()
