"""Explicit one-switch benchmark admission and the real comparison CLI; host-only fixtures."""
import copy
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
import benchlib  # noqa: E402

PRIMARY = 'TOTK_VK_REC_ONE_PRIMARY'


def set_state(doc, key, state, mode=None):
    record = {'state': state, 'reason': 'authenticated fixture',
              'evidence_version': benchlib.SWITCH_EVIDENCE_VERSION}
    if key == 'TOTK_MAP_WARN':
        record['mode'] = mode if mode is not None else (1 if state == 'on' else 0)
    doc['devices']['thor']['effective_switches'][key] = record


def document():
    return {
        'schema': benchlib.SCHEMA, 'name': 'fixture', 'build': {'commit': 'a' * 40},
        'devices': {'thor': {
            'name': 'AYN Thor', 'soc': 'QCS8550', 'settings_env': {'TOTK_FPS60': '0'},
            'effective_switches': benchlib.switch_states('off', 'authenticated fixture'),
            'freq_limits': benchlib.parse_freq_tokens(
                'cpu0=0,1:902400:2016000:2016000 gpu_maxclk=680000000 '
                'gpu_freqs=680000000,615000000 gpu_devfreq_max=680000000 gpu_max_pwrlevel=0'.split()),
        }},
        'runs': [{'device': 'thor', 'scene': 'hateno', 'thermal': [], 'freq_stable': True,
                  'summary': {'windows': 4, 'fps_mean': 30.0, 'fps_steady': 30.0, 'dip_windows': 0,
                              'dip_time_fraction': 0.0, 'p50_ms_median': 33.3, 'p95_ms_max': 38.0,
                              'gpu_ms_mean': 20.0, 'over50_total': 0}}],
    }


def pair(key='TOTK_MAP_WARN', reference='on', new='off'):
    base = document()
    set_state(base, key, reference)
    candidate = copy.deepcopy(base)
    set_state(candidate, key, new)
    return base, candidate


def signature(doc):
    return benchlib.device_signature(doc, 'thor')


def compare(base, new, expected_difference=None):
    out = io.StringIO()
    verdicts = benchlib.compare_docs(base, new, out, expected_difference=expected_difference)
    return verdicts[('thor', 'hateno')], out.getvalue()


class ExpectedDifferenceParsing(unittest.TestCase):
    def test_strings_and_tuples_accept_both_directions_for_every_supported_switch(self):
        for key in (*benchlib.SWITCH_KEYS, PRIMARY):
            for reference, new in (('on', 'off'), ('off', 'on')):
                expected = key, reference, new
                for value in (expected, ':'.join(expected)):
                    with self.subTest(value=value):
                        self.assertEqual(benchlib.parse_expected_difference(value), expected)

    def test_invalid_key_states_shape_and_equal_states_raise_value_error(self):
        for value in (None, 1, b'TOTK_MAP_WARN:on:off', ['TOTK_MAP_WARN', 'on', 'off'], (),
                      ('TOTK_MAP_WARN', 'on'), ('TOTK_MAP_WARN', 'on', 'off', 'extra'),
                      ('TOTK_MAP_WARN', True, 'off'), (None, 'on', 'off'),
                      '', 'TOTK_MAP_WARN', 'TOTK_MAP_WARN:on:off:extra', 'TOTK_FPS60:on:off',
                      'TOTK_MAP_WARN:UNKNOWN:off', 'TOTK_MAP_WARN:on:UNKNOWN',
                      'TOTK_MAP_WARN:1:0', 'TOTK_MAP_WARN:ON:off', 'TOTK_MAP_WARN:on:off ',
                      'TOTK_MAP_WARN:on:on', 'TOTK_MAP_WARN:off:off'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                benchlib.parse_expected_difference(value)

    def test_all_comparison_entrypoints_validate_declarations(self):
        base, new = pair()
        for call in (
            lambda: benchlib.switch_admission(base, 'thor', new, 'thor', 'bad:on:off'),
            lambda: benchlib.comparability(base, 'thor', new, 'thor', 'bad:on:off'),
            lambda: benchlib.signature_relation(signature(base), signature(new), 'bad:on:off'),
            lambda: benchlib.signature_relation(None, None, 'bad:on:off'),
            lambda: benchlib.compare_docs(base, new, io.StringIO(), expected_difference='bad:on:off'),
        ):
            with self.subTest(call=call), self.assertRaises(ValueError):
                call()


class ExpectedDifferenceAdmission(unittest.TestCase):
    def test_exact_transitions_are_admitted_without_mutating_documents_or_signatures(self):
        for key in (*benchlib.SWITCH_KEYS, PRIMARY):
            for reference, new_state in (('on', 'off'), ('off', 'on')):
                base, new = pair(key, reference, new_state)
                expected = key, reference, new_state
                before = copy.deepcopy((base, new))
                bsig, nsig = signature(base), signature(new)
                sig_before = copy.deepcopy((bsig, nsig))
                with self.subTest(expected=expected):
                    self.assertEqual(benchlib.switch_admission(base, 'thor', new, 'thor', expected), [])
                    self.assertEqual(benchlib.comparability(base, 'thor', new, 'thor', ':'.join(expected)), [])
                    self.assertEqual(benchlib.signature_relation(bsig, nsig, expected), 'match')
                    result, text = compare(base, new, ':'.join(expected))
                    self.assertEqual(result, ('same', 'match'))
                    declaration = 'Expected difference (reference -> new): ' + ':'.join(expected)
                    self.assertEqual(text.count(declaration), 1)
                    self.assertNotIn('NOT COMPARABLE', text)
                    self.assertEqual((base, new), before)
                    self.assertEqual((bsig, nsig), sig_before)
                    for sig in (bsig, nsig):
                        self.assertEqual(sig['sha256'], benchlib.signature_of_fields(sig['fields']))
                    if key in benchlib.SWITCH_KEYS:
                        self.assertNotEqual(bsig['sha256'], nsig['sha256'])
                    else:
                        self.assertEqual(bsig['sha256'], nsig['sha256'])

    def test_default_comparison_still_rejects_base_switch_differences(self):
        for key in benchlib.SWITCH_KEYS:
            base, new = pair(key)
            with self.subTest(key=key):
                self.assertTrue(benchlib.switch_admission(base, 'thor', new, 'thor'))
                self.assertEqual(benchlib.signature_relation(signature(base), signature(new)), 'mismatch')
                result, text = compare(base, new)
                self.assertEqual(result[1], 'mismatch')
                self.assertNotIn('Expected difference', text)
        base = document()
        self.assertEqual(compare(base, copy.deepcopy(base))[0], ('same', 'match'))

    def test_wrong_switch_and_reversed_or_identical_observed_states_are_rejected(self):
        base, new = pair()
        result, text = compare(base, new, 'TOTK_WB_ALIAS:on:off')
        self.assertEqual(result[1], 'mismatch')
        self.assertIn('expected difference TOTK_WB_ALIAS:on:off', text)
        self.assertIn('TOTK_MAP_WARN: on -> off', text)
        for reference, new_state in (('off', 'on'), ('on', 'on'), ('off', 'off')):
            base, new = pair('TOTK_MAP_WARN', reference, new_state)
            expected = 'TOTK_MAP_WARN:on:off'
            with self.subTest(reference=reference, new=new_state):
                reasons = benchlib.switch_admission(base, 'thor', new, 'thor', expected)
                self.assertTrue(any('expected difference' in reason for reason in reasons))
                self.assertEqual(benchlib.signature_relation(signature(base), signature(new), expected), 'mismatch')
                self.assertEqual(compare(base, new, expected)[0][1], 'mismatch')

    def test_every_extra_base_or_authenticated_optional_switch_difference_is_rejected(self):
        for selected in (*benchlib.SWITCH_KEYS, PRIMARY):
            for extra in (*benchlib.SWITCH_KEYS, PRIMARY):
                if selected == extra:
                    continue
                base = document()
                set_state(base, PRIMARY, 'off')
                new = copy.deepcopy(base)
                set_state(new, selected, 'on')
                set_state(new, extra, 'on')
                expected = selected, 'off', 'on'
                with self.subTest(selected=selected, extra=extra):
                    reasons = benchlib.switch_admission(base, 'thor', new, 'thor', expected)
                    self.assertIn(f'{extra}: off -> on', reasons)
                    self.assertEqual(benchlib.signature_relation(signature(base), signature(new), expected), 'mismatch')
                    self.assertEqual(compare(base, new, expected)[0][1], 'mismatch')

    def test_unknown_on_either_side_in_any_base_switch_is_never_admitted(self):
        for key in benchlib.SWITCH_KEYS:
            for side in ('reference', 'new'):
                base, new = pair()
                set_state(base if side == 'reference' else new, key, 'UNKNOWN')
                expected = 'TOTK_MAP_WARN:on:off'
                with self.subTest(key=key, side=side):
                    reasons = benchlib.switch_admission(base, 'thor', new, 'thor', expected)
                    self.assertTrue(any(f'{side} {key}=UNKNOWN' in reason for reason in reasons))
                    self.assertEqual(benchlib.signature_relation(signature(base), signature(new), expected), 'unknown')
                    self.assertEqual(compare(base, new, expected)[0][1], 'unknown')

    def test_map_modes_are_ignored_only_for_an_exact_selected_on_off_transition(self):
        for mode in (1, 2, 3):
            for reference, new_state in (('on', 'off'), ('off', 'on')):
                base, new = pair('TOTK_MAP_WARN', reference, new_state)
                set_state(base if reference == 'on' else new, 'TOTK_MAP_WARN', 'on', mode)
                expected = 'TOTK_MAP_WARN', reference, new_state
                with self.subTest(mode=mode, expected=expected):
                    self.assertEqual(benchlib.switch_admission(base, 'thor', new, 'thor', expected), [])
                    self.assertEqual(compare(base, new, expected)[0], ('same', 'match'))
        base, new = pair('TOTK_WB_ALIAS')
        set_state(base, 'TOTK_MAP_WARN', 'on', 1)
        set_state(new, 'TOTK_MAP_WARN', 'on', 2)
        for expected in ('TOTK_WB_ALIAS:on:off', 'TOTK_MAP_WARN:on:off'):
            with self.subTest(expected=expected):
                reasons = benchlib.switch_admission(base, 'thor', new, 'thor', expected)
                self.assertIn('TOTK_MAP_WARN mode: 1 -> 2', reasons)
                self.assertEqual(compare(base, new, expected)[0][1], 'mismatch')
        for mode in (None, True, '1', 0, 4):
            base, new = pair()
            base['devices']['thor']['effective_switches']['TOTK_MAP_WARN']['mode'] = mode
            with self.subTest(invalid_mode=mode):
                self.assertEqual(compare(base, new, 'TOTK_MAP_WARN:on:off')[0][1], 'unknown')

    def test_all_non_switch_signature_fields_remain_significant(self):
        changes = {
            'model': lambda dev: dev.update(name='another model'),
            'soc': lambda dev: dev.update(soc='another SoC'),
            'settings_env_sha256': lambda dev: dev['settings_env'].update(TOTK_FPS60='1'),
            'cpu_caps_khz': lambda dev: dev['freq_limits']['cpu'][0].update(scaling_max_khz=1804800),
            'gpu_cap_hz': lambda dev: dev['freq_limits']['gpu'].update(cap_hz=615000000),
        }
        self.assertEqual(set(changes), set(benchlib.SIGNATURE_FIELDS))
        for selected in ('TOTK_MAP_WARN', PRIMARY):
            for field, change in changes.items():
                base, new = pair(selected)
                change(new['devices']['thor'])
                before = copy.deepcopy((base, new))
                expected = selected, 'on', 'off'
                with self.subTest(selected=selected, field=field):
                    self.assertEqual(benchlib.switch_admission(base, 'thor', new, 'thor', expected), [])
                    self.assertEqual(benchlib.signature_relation(signature(base), signature(new), expected), 'mismatch')
                    result, text = compare(base, new, expected)
                    self.assertEqual(result[1], 'mismatch')
                    self.assertIn(field + ':', text)
                    self.assertIn('NOT COMPARABLE', text)
                    self.assertEqual((base, new), before)

    def test_only_the_declared_treatment_env_entry_is_ignored_without_changing_raw_signatures(self):
        for key in (*benchlib.SWITCH_KEYS, PRIMARY):
            for reference, new_state in (('off', 'on'), ('on', 'off')):
                base, new = pair(key, reference, new_state)
                off_doc = base if reference == 'off' else new
                off_doc['devices']['thor']['settings_env'][key] = '0'
                expected = key, reference, new_state
                bsig, nsig = signature(base), signature(new)
                before = copy.deepcopy((base, new, bsig, nsig))
                with self.subTest(expected=expected):
                    self.assertNotEqual(bsig['sha256'], nsig['sha256'])
                    self.assertNotEqual(bsig['fields']['settings_env_sha256'], nsig['fields']['settings_env_sha256'])
                    self.assertEqual(benchlib.signature_relation(bsig, nsig, expected), 'match')
                    self.assertEqual(benchlib.comparability(base, 'thor', new, 'thor', expected), [])
                    result, text = compare(base, new, expected)
                    self.assertEqual(result, ('same', 'match'))
                    self.assertNotIn('WARNING', text)
                    self.assertEqual(benchlib.signature_relation(bsig, nsig), 'mismatch')
                    self.assertEqual(compare(base, new)[0][1], 'mismatch')
                    self.assertTrue(any('settings_env differs' in reason for reason in
                                        benchlib.comparability(base, 'thor', new, 'thor')))
                    self.assertEqual((base, new, bsig, nsig), before)
                    for sig in (bsig, nsig):
                        self.assertEqual(sig['sha256'], benchlib.signature_of_fields(sig['fields']))

    def test_declared_env_normalization_keeps_every_unrelated_setting_strict(self):
        for extra in ('TOTK_FPS60', 'TOTK_MENU_CAPTURE_FIX', 'TOTK_DATA_DIR'):
            base, new = pair(PRIMARY, 'off', 'on')
            base['devices']['thor']['settings_env'][PRIMARY] = '0'
            new['devices']['thor']['settings_env'][extra] = 'changed'
            expected = PRIMARY + ':off:on'
            with self.subTest(extra=extra):
                self.assertEqual(benchlib.switch_admission(base, 'thor', new, 'thor', expected), [])
                self.assertEqual(benchlib.signature_relation(signature(base), signature(new), expected), 'mismatch')
                self.assertEqual(compare(base, new, expected)[0][1], 'mismatch')
                warns = benchlib.comparability(base, 'thor', new, 'thor', expected)
                self.assertTrue(any(extra + ':' in warning for warning in warns))
                self.assertFalse(any(PRIMARY + ':' in warning for warning in warns))

    def test_env_normalization_requires_raw_env_on_both_signatures(self):
        base, new = pair(PRIMARY, 'off', 'on')
        base['devices']['thor']['settings_env'][PRIMARY] = '0'
        for missing in ('reference', 'new', 'both'):
            bsig, nsig = signature(base), signature(new)
            if missing in ('reference', 'both'):
                del bsig['settings_env']
            if missing in ('new', 'both'):
                del nsig['settings_env']
            with self.subTest(missing=missing):
                self.assertEqual(benchlib.signature_relation(bsig, nsig, PRIMARY + ':off:on'), 'mismatch')

    def test_reference_only_allows_only_the_declared_env_entry_difference(self):
        base, new = pair(PRIMARY, 'off', 'on')
        base['devices']['thor']['settings_env'][PRIMARY] = '0'
        base['devices']['thor']['freq_limits']['gpu']['cap_hz'] = None
        expected = PRIMARY + ':off:on'
        self.assertEqual(compare(base, new, expected)[0][1], 'reference-only')
        new['devices']['thor']['settings_env']['TOTK_FPS60'] = '1'
        self.assertEqual(compare(base, new, expected)[0][1], 'mismatch')

    def test_reference_only_still_requires_all_other_signature_fields_to_match(self):
        base, new = pair()
        base['devices']['thor']['freq_limits']['gpu']['cap_hz'] = None
        expected = 'TOTK_MAP_WARN:on:off'
        self.assertEqual(compare(base, new, expected)[0][1], 'reference-only')
        new['devices']['thor']['soc'] = 'another SoC'
        self.assertEqual(compare(base, new, expected)[0][1], 'mismatch')

    def test_other_comparability_warnings_and_missing_signatures_are_retained(self):
        base, new = pair()
        new['devices']['thor']['settings_env']['TOTK_FPS60'] = '1'
        new['runs'][0]['thermal'] = [{'cpufreq_caps_khz': [{'scaling_max': 1, 'cpuinfo_max': 2}]}]
        new['runs'][0]['freq_stable'] = False
        warns = benchlib.comparability(base, 'thor', new, 'thor', 'TOTK_MAP_WARN:on:off')
        for fragment in ('settings_env differs', 'cpufreq caps differ', 'freq_stable false'):
            self.assertTrue(any(fragment in warning for warning in warns))
        base, new = pair()
        del new['devices']['thor']['freq_limits']
        result, text = compare(base, new, 'TOTK_MAP_WARN:on:off')
        self.assertEqual(result[1], 'unknown')
        self.assertIn('Expected difference (reference -> new): TOTK_MAP_WARN:on:off', text)

    def test_optional_metadata_preserves_default_hashes_fields_and_admission(self):
        base, new = pair(PRIMARY)
        plain = signature(document())
        self.assertEqual(benchlib.SWITCH_KEYS, ('TOTK_WB_ALIAS', 'TOTK_MAP_WARN', 'TOTK_MENU_CAPTURE_FIX'))
        self.assertNotIn('optional_switches', plain)
        for doc in (base, new):
            sig = signature(doc)
            self.assertEqual(sig['fields'], plain['fields'])
            self.assertEqual(sig['sha256'], plain['sha256'])
            self.assertEqual(sig['optional_switches'][PRIMARY],
                             doc['devices']['thor']['effective_switches'][PRIMARY])
        self.assertEqual(benchlib.switch_admission(base, 'thor', new, 'thor'), [])
        self.assertEqual(compare(base, new)[0], ('same', 'match'))

    def test_missing_obsolete_or_malformed_selected_optional_metadata_never_means_off(self):
        for record in (None, 'off', {'state': 'off', 'evidence_version': 3},
                       {'state': 'bad', 'evidence_version': benchlib.SWITCH_EVIDENCE_VERSION},
                       {'state': 'UNKNOWN', 'evidence_version': benchlib.SWITCH_EVIDENCE_VERSION}):
            for side in ('reference', 'new'):
                base, new = pair(PRIMARY)
                states = (base if side == 'reference' else new)['devices']['thor']['effective_switches']
                if record is None:
                    del states[PRIMARY]
                else:
                    states[PRIMARY] = record
                expected = PRIMARY + ':on:off'
                with self.subTest(record=record, side=side):
                    reasons = benchlib.switch_admission(base, 'thor', new, 'thor', expected)
                    self.assertTrue(any(f'{side} {PRIMARY}=UNKNOWN' in reason for reason in reasons))
                    self.assertEqual(benchlib.signature_relation(signature(base), signature(new), expected), 'unknown')
                    self.assertEqual(compare(base, new, expected)[0][1], 'unknown')

    def test_optional_startup_records_are_resolved_only_when_actually_present(self):
        records = {key: [{'line': 2, 'kind': 'marker', 'state': 'off',
                          **({'mode': 0} if key == 'TOTK_MAP_WARN' else {})}]
                   for key in benchlib.SWITCH_KEYS}
        capture = {'evidence_version': benchlib.SWITCH_EVIDENCE_VERSION,
                   'starts': [1], 'presents': [3], 'records': records}
        self.assertNotIn(PRIMARY, benchlib.log_switch_states({'switch_capture': capture}, {}))
        records[PRIMARY] = []
        self.assertNotIn(PRIMARY, benchlib.log_switch_states({'switch_capture': capture}, {}))
        for state in ('on', 'off'):
            records[PRIMARY] = [{'line': 2, 'kind': 'marker', 'state': state}]
            before = copy.deepcopy(capture)
            resolved = benchlib.log_switch_states({'switch_capture': capture}, {})
            self.assertEqual(resolved[PRIMARY]['state'], state)
            self.assertEqual(resolved[PRIMARY]['evidence_version'], benchlib.SWITCH_EVIDENCE_VERSION)
            self.assertEqual(capture, before)
        records[PRIMARY].append({'line': 2, 'kind': 'marker', 'state': 'on'})
        self.assertEqual(benchlib.log_switch_states({'switch_capture': capture}, {})[PRIMARY]['state'], 'UNKNOWN')
        records[PRIMARY] = [{'line': 2, 'kind': 'marker', 'state': 'on'}]
        capture['presents'] = []
        self.assertEqual(benchlib.log_switch_states({'switch_capture': capture}, {})[PRIMARY]['state'], 'UNKNOWN')


class ExpectedDifferenceCLI(unittest.TestCase):
    def run_compare(self, base, new, *args, output=False):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            for name, doc in (('reference', base), ('new', new)):
                (work / f'{name}.json').write_text(json.dumps(doc), encoding='utf-8')
            command = [sys.executable, str(BENCH / 'compare.py'), str(work / 'reference.json'),
                       str(work / 'new.json'), *args]
            if output:
                command += ['--out', str(work / 'comparison.txt')]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            written = (work / 'comparison.txt').read_text(encoding='utf-8') if output else None
        return result, written

    def test_cli_declaration_admits_regression_and_is_recorded_in_stdout_and_out_file(self):
        for key in ('TOTK_MAP_WARN', PRIMARY):
            base, new = pair(key)
            new['runs'][0]['summary']['fps_steady'] = 20.0
            declaration = key + ':on:off'
            result, written = self.run_compare(base, new, '--expected-difference', declaration, output=True)
            with self.subTest(key=key):
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertEqual(result.stderr, '')
                self.assertEqual(written, result.stdout)
                self.assertIn('Expected difference (reference -> new): ' + declaration, result.stdout)
                self.assertIn('frequency signatures match', result.stdout)
                self.assertIn('scenes with matching signatures: 0 better, 1 worse, 0 same', result.stdout)
                self.assertNotIn('NOT COMPARABLE', result.stdout)
        result, _ = self.run_compare(*pair())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('NOT COMPARABLE', result.stdout)
        self.assertNotIn('Expected difference', result.stdout)

    def test_cli_explicit_optout_vs_absent_default_requires_declaration_and_preserves_other_settings(self):
        base, new = pair(PRIMARY, 'off', 'on')
        base['devices']['thor']['settings_env'][PRIMARY] = '0'
        new['runs'][0]['summary']['fps_steady'] = 20.0
        result, _ = self.run_compare(base, new, '--expected-difference', PRIMARY + ':off:on')
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('frequency signatures match', result.stdout)
        self.assertNotIn('settings_env differs', result.stdout)
        result, _ = self.run_compare(base, new)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('NOT COMPARABLE', result.stdout)
        self.assertIn('settings_env differs', result.stdout)
        new['devices']['thor']['settings_env']['TOTK_FPS60'] = '1'
        result, _ = self.run_compare(base, new, '--expected-difference', PRIMARY + ':off:on')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('NOT COMPARABLE', result.stdout)
        self.assertIn('settings_env differs (TOTK_FPS60: 0 -> 1)', result.stdout)

    def test_cli_rejects_undeclared_differences_and_keeps_non_switch_mismatches(self):
        for extra in ('TOTK_MENU_CAPTURE_FIX', 'settings_env'):
            base, new = pair()
            new['runs'][0]['summary']['fps_steady'] = 20.0
            if extra == 'settings_env':
                new['devices']['thor']['settings_env']['TOTK_FPS60'] = '1'
            else:
                set_state(new, extra, 'on')
            result, _ = self.run_compare(base, new, '--expected-difference', 'TOTK_MAP_WARN:on:off')
            with self.subTest(extra=extra):
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('NOT COMPARABLE', result.stdout)
                self.assertIn('0 better, 0 worse, 0 same', result.stdout)
                self.assertIn('Expected difference (reference -> new): TOTK_MAP_WARN:on:off', result.stdout)

    def test_cli_invalid_declarations_are_argparse_errors_without_tracebacks(self):
        for value in ('bad:on:off', 'TOTK_MAP_WARN:UNKNOWN:off', 'TOTK_MAP_WARN:on:on', 'TOTK_MAP_WARN:on'):
            result, _ = self.run_compare(*pair(), '--expected-difference', value)
            with self.subTest(value=value):
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertEqual(result.stdout, '')
                self.assertIn('usage:', result.stderr)
                self.assertIn('--expected-difference', result.stderr)
                self.assertNotIn('Traceback', result.stderr)


if __name__ == '__main__':
    unittest.main()
