#!/usr/bin/env python3
"""Analyze schema-3 GPU-profiler JSONL (host only, Python standard library).

Pass gpu_ms and counters are window SUMS, not per-frame averages. Scene-relative
families use exact scene ratios; other families retain absolute attachment areas.
Long-frame rows are a separate, selected population and never added to windows.
"""
import argparse
import glob
from fractions import Fraction
import json
import math
from pathlib import Path
import re
import sys

from benchlib import guard_output

STATS = ('ia_vertices', 'ia_primitives', 'vs_invocations', 'clipping_invocations',
         'clipping_primitives', 'fs_invocations')
ATTACHMENT = ('slot', 'format', 'width', 'height', 'load', 'store')
ATTACHMENT_RATIOS = ('width_num', 'width_den', 'height_num', 'height_den')
REFERENCE = ('reference_width', 'reference_height', 'scene_relative')
UINT32_MAX = (1 << 32) - 1
WINDOW_FRAMES = 300
SPAN_LIMIT_MS = 10000
RATIOS = ('verts_per_draw', 'prims_per_draw', 'fragments_per_prim', 'ns_per_vertex', 'ns_per_primitive')


def number(row, key, integer=False, minimum: float = 0):
    value = row.get(key)
    if isinstance(value, bool) or not isinstance(value, int if integer else (int, float)):
        raise ValueError(f'{key}: expected {"integer" if integer else "number"}')
    try:
        finite = integer or math.isfinite(value)
    except OverflowError:
        finite = False
    if value < minimum or not finite:
        raise ValueError(f'{key}: expected finite value >= {minimum}')
    return value


def dimensions(row, prefix, minimum=0):
    width, height = (number(row, f'{prefix}_{axis}', True, minimum) for axis in ('width', 'height'))
    if width > UINT32_MAX or height > UINT32_MAX:
        raise ValueError(f'{prefix}: dimensions exceed uint32')
    if bool(width) != bool(height):
        raise ValueError(f'{prefix}: zero dimensions must be paired')
    return width, height


def validate(row):
    """Reject malformed v3 rows; preserve additive producer metadata verbatim."""
    if not isinstance(row, dict) or type(row.get('schema')) is not int or row['schema'] != 3:
        raise ValueError('expected object with schema=3')
    kind = row.get('type')
    if kind == 'header':
        if not isinstance(row.get('launch_id'), str) or not row['launch_id']:
            raise ValueError('launch_id: expected nonempty string')
        number(row, 'rotation', True)
        for key in ('ordinals', 'stats'):
            if not isinstance(row.get(key), bool):
                raise ValueError(f'{key}: expected boolean')
        for key in ('profile', 'max_bytes', 'retained_files', 'window_frames'):
            number(row, key, True, 1)
        number(row, 'span_limit_ms', minimum=1)
        if not isinstance(row.get('driver'), dict) or not isinstance(row.get('env'), dict):
            raise ValueError('header requires driver and env objects')
        return row
    if kind == 'session_end':
        if not isinstance(row.get('launch_id'), str) or not row['launch_id']:
            raise ValueError('launch_id: expected nonempty string')
        retired = row.get('all_queries_retired')
        if row.get('clean_end') is not True or 'all_queries_retired' not in row or (
                retired is not None and not isinstance(retired, bool)):
            raise ValueError('session_end requires clean_end=true and nullable all_queries_retired')
        for key in ('last_frame', 'last_window_sequence', 'last_window_end', 'output_failures'):
            number(row, key, True)
        return row
    if kind not in ('window', 'pass', 'frame', 'long_frame'):
        raise ValueError('unknown row type')
    if kind in ('window', 'pass'):
        start, end = (number(row, k, True, 1) for k in ('window_start', 'window_end'))
        if end < start:
            raise ValueError('window_end precedes window_start')
    if kind in ('frame', 'long_frame'):
        number(row, 'frame', True, 1)
    if kind == 'frame':
        dimensions(row, 'scene')
    if kind in ('window', 'frame'):
        valid = row.get('span_valid', True)
        if not isinstance(valid, bool):
            raise ValueError('span_valid: expected boolean')
        number(row, 'measured_ms')
        for key in ('span_ms', 'gap_ms'):
            if valid:
                number(row, key, minimum=-math.inf if key == 'gap_ms' else 0)
            elif key not in row or row[key] is not None:
                raise ValueError(f'{key}: invalid span must be present and null')
        if row.get('raw_span_ms') is not None:
            number(row, 'raw_span_ms')
        for key in ('valid_span_ms', 'valid_gap_ms'):
            if key in row:
                number(row, key, minimum=-math.inf if key == 'valid_gap_ms' else 0)
        if 'span_reason' in row and (not isinstance(row['span_reason'], str) or not row['span_reason']):
            raise ValueError('span_reason: expected nonempty string')
        number(row, 'dropped', True)
        for key in ('failed_slots', 'stats_failed_slots', 'frames', 'invalid_spans', 'span_frames',
                    'unavailable_timestamps', 'invalid_timestamps', 'timestamp_wraps', 'output_failures'):
            if key in row:
                number(row, key, True)
        if 'sequence' in row:
            if number(row, 'sequence', True, 1) != (row['window_start'] - 1) // WINDOW_FRAMES + 1:
                raise ValueError('sequence differs from window_start')
        for key in ('partial', 'reconciliation_failed', 'ordinals'):
            if key in row and not isinstance(row[key], bool):
                raise ValueError(f'{key}: expected boolean')
    else:
        for key in ('key', 'shader', 'kind'):
            if not isinstance(row.get(key), str) or not row[key]:
                raise ValueError(f'{key}: expected nonempty string')
        if row['kind'] not in ('render', 'compute', 'upload', 'transfer', 'post', 'upscale', 'present', 'framegen'):
            raise ValueError('kind: unknown GPU scope kind')
        dimensions(row, 'render', 1 if row['kind'] == 'render' else 0)
        dimensions(row, 'reference', 1)
        if not isinstance(row.get('scene_relative'), bool):
            raise ValueError('scene_relative: expected boolean')
        number(row, 'ordinal', True, 1 if kind == 'long_frame' else 0)
        number(row, 'gpu_ms')
        count = number(row, 'count', True, 1)
        number(row, 'draws', True)
        number(row, 'samples', True)
        valid = number(row, 'stats_valid', True)
        if valid > count:
            raise ValueError('stats_valid exceeds count')
        if ('stats' in row) != bool(valid):
            raise ValueError('stats must be present exactly when stats_valid > 0')
        if valid:
            if not isinstance(row['stats'], dict):
                raise ValueError('stats: expected object')
            for key in STATS:
                number(row['stats'], key, True)
        if ('stats_gpu_ms' in row) != ('stats_draws' in row):
            raise ValueError('stats_gpu_ms and stats_draws must be paired')
        if 'stats_gpu_ms' in row:
            if not valid:
                raise ValueError('matched statistics require stats_valid > 0')
            if number(row, 'stats_gpu_ms') > row['gpu_ms'] + 0.01:
                raise ValueError('stats_gpu_ms exceeds gpu_ms')
            if number(row, 'stats_draws', True) > row['draws']:
                raise ValueError('stats_draws exceeds draws')
        if 'ratios' in row:
            if not valid or not isinstance(row['ratios'], dict):
                raise ValueError('ratios require available stats')
            for key, value in row['ratios'].items():
                if key in RATIOS and value is not None:
                    number(row['ratios'], key)
        attachments = row.get('attachments')
        if not isinstance(attachments, list):
            raise ValueError('attachments: expected list')
        slots = set()
        for attachment in attachments:
            if not isinstance(attachment, dict):
                raise ValueError('attachment: expected object')
            for key in (*ATTACHMENT, *ATTACHMENT_RATIOS):
                number(attachment, key, True, 1 if key in ('width', 'height') else 0)
            if attachment['width'] > UINT32_MAX or attachment['height'] > UINT32_MAX:
                raise ValueError('attachment: dimensions exceed uint32')
            if attachment['slot'] in slots:
                raise ValueError('duplicate attachment slot')
            slots.add(attachment['slot'])
            for axis in ('width', 'height'):
                n, d = (attachment[f'{axis}_{part}'] for part in ('num', 'den'))
                if d == 0 or math.gcd(n, d) != 1:
                    raise ValueError(f'{axis}: expected reduced rational with positive denominator')
                if n * row[f'reference_{axis}'] != attachment[axis] * d:
                    raise ValueError(f'{axis}: rational differs from attachment/reference dimensions')
    return row


def stream_path(path):
    """One producer launch plus its .1/.2/.3 rotations is one observation stream."""
    return re.sub(r'(?<=\.jsonl)\.[1-3]$', '', str(path.resolve()))


def inputs(patterns):
    paths = {}
    for pattern in patterns:
        matches = sorted(glob.glob(str(pattern)))
        if not matches:
            raise ValueError(f'no input files match {pattern!r}')
        for name in matches:
            path = Path(name).resolve()
            if not path.is_file():
                raise ValueError(f'not a file: {name}')
            paths[path] = None
    # Oldest rotation first, current file last. Aggregation never relies on order.
    return sorted(paths, key=lambda p: (stream_path(p), -int(p.suffix[1:]) if p.suffix[1:].isdigit() else 0))


def identity(stream, row):
    kind = row['type']
    if kind == 'header':
        fields = ('launch_id', 'rotation')
    elif kind == 'session_end':
        fields = ('launch_id',)
    elif kind == 'window':
        fields = ('window_start', 'window_end')
    elif kind == 'pass':
        fields = ('window_start', 'window_end', 'key', 'ordinal', *REFERENCE)
    elif kind == 'frame':
        fields = ('frame',)
    else:
        fields = ('frame', 'key', 'ordinal', *REFERENCE)
    return (stream, kind, *(row[key] for key in fields))


def load(paths, legacy_span_replay=False):
    def reject_constant(text):
        raise ValueError(f'nonfinite JSON number {text}')

    observations, conflicts = {}, set()
    issues = []
    counts = dict(malformed_rows=0, truncated_rows=0, duplicate_rows=0, conflicting_rows=0, blank_lines=0,
                  legacy_ignored_rows=0)
    for path in paths:
        stream = stream_path(path)
        with path.open(encoding='utf-8', errors='replace') as file:
            for line_no, line in enumerate(file, 1):
                if not line.strip():
                    counts['blank_lines'] += 1
                    continue
                where = f'{path}:{line_no}'
                try:
                    row = json.loads(line, parse_constant=reject_constant)
                    if legacy_span_replay and isinstance(row, dict) and row.get('schema') in (2, 3):
                        if row.get('type') not in ('frame', 'window', 'header', 'session_end'):
                            counts['legacy_ignored_rows'] += 1
                            continue
                        if row['schema'] == 2:
                            row = dict(row, schema=3, source_schema=2)
                            if row['type'] == 'frame':
                                row.setdefault('scene_width', 0)
                                row.setdefault('scene_height', 0)
                    row = validate(row)
                except (ValueError, TypeError) as error:
                    # A writer may still own the final line. Do not silently accept
                    # it, but distinguish that censored tail from an interior error.
                    tail = (isinstance(error, json.JSONDecodeError) and not line.endswith('\n')
                            and line.lstrip().startswith('{') and not line.rstrip().endswith('}'))
                    counts['truncated_rows' if tail else 'malformed_rows'] += 1
                    issues.append(f'{where}: {"incomplete tail" if tail else "malformed row"}: {error}')
                    continue
                ident = identity(stream, row)
                if ident in conflicts:
                    counts['conflicting_rows'] += 1
                elif ident in observations:
                    if observations[ident]['row'] == row:
                        counts['duplicate_rows'] += 1
                    else:
                        counts['conflicting_rows'] += 1
                        issues.append(f'{where}: conflicting identity; excluding both observations {ident[1:]}')
                        del observations[ident]
                        conflicts.add(ident)
                else:
                    observations[ident] = {'stream': stream, 'source': where, 'row': row}
    return list(observations.values()), counts, issues


def absolute_area(row):
    attachments = sorted(row['attachments'], key=lambda a: a['slot'])
    return (row['render_width'], row['render_height'],
            tuple(tuple(a[k] for k in ATTACHMENT) for a in attachments))


def family(row):
    if not row['scene_relative']:
        area = absolute_area(row)
    else:
        attachments = sorted(row['attachments'], key=lambda a: a['slot'])
        area = (Fraction(row['render_width'], row['reference_width']),
                Fraction(row['render_height'], row['reference_height']),
                tuple((a['slot'], a['format'], *(a[k] for k in ATTACHMENT_RATIOS), a['load'], a['store'])
                      for a in attachments))
    return (row['kind'], row['samples'], row['scene_relative'], area)


def ratio(numerator, denominator):
    return numerator / denominator if denominator else None


def aggregate(rows, per_family=False):
    groups = {}
    for row in rows:
        absolute = absolute_area(row)
        ident = family(row) if per_family else (
            row['key'], row['ordinal'], row['kind'], row['samples'], absolute, row['shader'])
        if ident not in groups:
            groups[ident] = dict(kind=row['kind'], samples=row['samples'], areas={},
                                 keys=set(), ordinals=set(), shaders=set(), rows=0, count=0, draws=0,
                                 gpu_ms=0.0, stats_valid=0, stats_missing=0, stats_partial_rows=0,
                                 stats={key: 0 for key in STATS}, ratio_draws=0, ratio_gpu_ms=0.0,
                                 ratio_stats={key: 0 for key in STATS})
            if per_family:
                groups[ident]['scene_relative'] = row['scene_relative']
            else:
                groups[ident].update(render_width=row['render_width'], render_height=row['render_height'],
                                     attachments=[{k: a[k] for k in ATTACHMENT}
                                                  for a in sorted(row['attachments'], key=lambda a: a['slot'])])
        group = groups[ident]
        variant = (absolute, *(row[k] for k in REFERENCE))
        if variant not in group['areas']:
            group['areas'][variant] = dict(
                keys=set(), render_width=row['render_width'], render_height=row['render_height'],
                **{k: row[k] for k in REFERENCE},
                attachments=sorted(row['attachments'], key=lambda a: a['slot']))
        group['areas'][variant]['keys'].add(row['key'])
        group['keys'].add(row['key'])
        group['ordinals'].add(row['ordinal'])
        group['shaders'].add(row['shader'])
        group['rows'] += 1
        for key in ('count', 'draws', 'gpu_ms', 'stats_valid'):
            group[key] += row[key]
        group['stats_missing'] += row['count'] - row['stats_valid']
        if row['stats_valid']:
            for key in STATS:
                group['stats'][key] += row['stats'][key]
            if row['stats_valid'] == row['count'] or 'stats_gpu_ms' in row:
                group['ratio_draws'] += row.get('stats_draws', row['draws'])
                group['ratio_gpu_ms'] += row.get('stats_gpu_ms', row['gpu_ms'])
                for key in STATS:
                    group['ratio_stats'][key] += row['stats'][key]
            if row['stats_valid'] < row['count']:
                group['stats_partial_rows'] += 1
    result = []
    for group in groups.values():
        group['areas'] = [{**area, 'keys': sorted(area['keys'])}
                          for _, area in sorted(group['areas'].items())]
        for key in ('keys', 'ordinals', 'shaders'):
            group[key] = sorted(group[key])
        if not per_family:
            group['key'], group['ordinal'], group['shader'] = (
                group['keys'][0], group['ordinals'][0], group['shaders'][0])
        group['draws_per_pass'] = ratio(group['draws'], group['count'])
        group['ns_per_draw'] = ratio(group['gpu_ms'] * 1e6, group['draws'])
        stats = group['stats']
        matched = group.pop('ratio_stats')
        group['ratios'] = {
            'verts_per_draw': ratio(matched['ia_vertices'], group['ratio_draws']),
            'prims_per_draw': ratio(matched['ia_primitives'], group['ratio_draws']),
            'fragments_per_prim': ratio(stats['fs_invocations'], stats['ia_primitives']),
            'ns_per_vertex': ratio(group['ratio_gpu_ms'] * 1e6, matched['ia_vertices']),
            'ns_per_primitive': ratio(group['ratio_gpu_ms'] * 1e6, matched['ia_primitives']),
        } if group['stats_valid'] else None
        if not group['stats_valid']:
            group['stats'] = None
        result.append(group)
    return sorted(result, key=lambda g: (-g['gpu_ms'], g['keys'], g['ordinals']))


def retention(observations, issues):
    """Use producer sequence/rotation facts, never infer a clean process exit."""
    streams = {}
    for observation in observations:
        streams.setdefault(observation['stream'], []).append(observation['row'])
    sessions = []
    for stream, rows in sorted(streams.items()):
        headers = sorted((r for r in rows if r['type'] == 'header'), key=lambda r: r['rotation'])
        ends = [r for r in rows if r['type'] == 'session_end']
        summaries = [r for r in rows if r['type'] == 'window']
        sequences = sorted({r.get('sequence', (r['window_start'] - 1) // WINDOW_FRAMES + 1)
                            for r in summaries})
        rotations = sorted({r['rotation'] for r in headers})
        frame_ids = [r['frame'] for r in rows if r['type'] == 'frame']
        proven_completed = (max(frame_ids, default=1) - 1) // WINDOW_FRAMES
        last = max([proven_completed, *sequences, *(r['last_window_sequence'] for r in ends)], default=0)
        # Without a header, old captures only prove holes within captured bounds.
        first = 1 if headers else min([*sequences, *((f - 1) // WINDOW_FRAMES + 1 for f in frame_ids)], default=1)
        missing = max(0, last - first + 1 - len(sequences))
        evictions = max(0, max(rotations, default=0) - 3)
        missing_files = max(rotations, default=-1) + 1 - len(rotations)
        header_conflict = bool(headers and any(
            {k: v for k, v in h.items() if k != 'rotation'} !=
            {k: v for k, v in headers[0].items() if k != 'rotation'} for h in headers[1:]))
        launch_ids = {r['launch_id'] for r in (*headers, *ends)}
        if header_conflict or len(launch_ids) > 1:
            issues.append(f'{stream}: inconsistent launch headers/session end')
        if missing:
            issues.append(f'{stream}: {missing} missing complete window sequence(s)')
        if evictions:
            issues.append(f'{stream}: rotation index proves {evictions} earlier file(s) exceeded retention')
        if missing_files:
            issues.append(f'{stream}: {missing_files} rotation file(s) absent from this capture')
        if headers and not ends:
            issues.append(f'{stream}: no session_end; process may be running, crashed, killed or capture incomplete')
        if ends and not headers:
            issues.append(f'{stream}: session_end present without a retained header')
        output_failures = max((r.get('output_failures', 0) for r in rows), default=0)
        if output_failures:
            issues.append(f'{stream}: producer reports {output_failures} cumulative output failure(s)')
        sessions.append(dict(stream=stream, headers=headers, end=ends[0] if len(ends) == 1 else None,
                             clean_end=bool(len(ends) == 1 and len(launch_ids) == 1 and not header_conflict),
                             missing_windows=missing, rotation_evictions=evictions,
                             missing_rotation_files=missing_files, header_conflict=header_conflict,
                             output_failures=output_failures,
                             leading_coverage_known=bool(headers), all_queries_retired=
                             ends[0].get('all_queries_retired') if len(ends) == 1 else None))
    return sessions


def span_analysis(by_type, issues):
    invalid_frames = set()
    rejected = []
    timing = {}
    reconciliation_failures = 0
    for kind in ('frame', 'window'):
        valid_span = valid_gap = measured = 0.0
        invalid = valid = 0
        for observation in by_type[kind]:
            row = observation['row']
            measured += row['measured_ms']
            span, gap = row['span_ms'], row['gap_ms']
            frames = 1 if kind == 'frame' else row.get('frames', row['window_end'] - row['window_start'] + 1)
            reason = None
            if row.get('span_valid') is False:
                reason = row.get('span_reason') or 'producer-invalid-span'
            elif span is not None and span > SPAN_LIMIT_MS * max(1, frames):
                reason = 'implausible-timestamp'
            elif span is None or gap is None:
                reason = 'missing-endpoints'
            elif gap < -0.01:
                reason = 'negative-gap'
            elif kind == 'window' and any(
                    stream == observation['stream'] and row['window_start'] <= frame <= row['window_end']
                    for stream, frame in invalid_frames):
                reason = 'contains-invalid-frame-span'
            if reason:
                invalid += 1
                if kind == 'frame':
                    invalid_frames.add((observation['stream'], row['frame']))
                valid_span += row.get('valid_span_ms', 0)
                valid_gap += row.get('valid_gap_ms', 0)
                rejected.append(dict(source=observation['source'], type=kind, reason=reason,
                                     frame=row.get('frame'), window_start=row.get('window_start'),
                                     raw_span_ms=row.get('raw_span_ms', span),
                                     measured_ms=row['measured_ms'], source_schema=row.get('source_schema', 3),
                                     endpoint_validity='not reconstructible from aggregate capture',
                                     timestamps={key: row[key] for key in (
                                         'first_tick', 'last_tick', 'timestamp_bits', 'timestamp_period',
                                         'timestamp_period_ns', 'queue_family', 'unavailable_timestamps',
                                         'invalid_timestamps', 'timestamp_wraps', 'span_reason') if key in row}))
                continue
            assert span is not None and gap is not None
            valid += 1
            valid_span += span
            valid_gap += gap
            if not math.isclose(span, row['measured_ms'] + gap, abs_tol=0.01, rel_tol=1e-5):
                reconciliation_failures += 1
                issues.append(f'{observation["source"]}: span != measured + gap')
        timing[kind] = dict(measured_ms=measured, span_ms=None if invalid else valid_span,
                            gap_ms=None if invalid else valid_gap, valid_span_ms=valid_span,
                            valid_gap_ms=valid_gap, valid_rows=valid, invalid_rows=invalid)
    if rejected:
        issues.append(f'{len(rejected)} invalid/implausible frame or window spans excluded; measured pass sums preserved')
    return timing, rejected, invalid_frames, reconciliation_failures


def analyze(paths, legacy_span_replay=False):
    paths = inputs(paths)
    observations, counts, issues = load(paths, legacy_span_replay)
    by_type = {kind: [] for kind in ('header', 'session_end', 'window', 'pass', 'frame', 'long_frame')}
    for observation in observations:
        by_type[observation['row']['type']].append(observation)
    windows = {(o['stream'], o['row']['window_start'], o['row']['window_end']): o['row']
               for o in by_type['window']}
    pass_sums = {}
    for observation in by_type['pass']:
        row = observation['row']
        ident = (observation['stream'], row['window_start'], row['window_end'])
        pass_sums[ident] = pass_sums.get(ident, 0.0) + row['gpu_ms']
    missing_windows = sorted(set(pass_sums) - set(windows))
    mismatches = [] if legacy_span_replay else [
        ident for ident, row in windows.items()
        if not math.isclose(pass_sums.get(ident, 0.0), row['measured_ms'], abs_tol=0.01, rel_tol=1e-5)]
    if missing_windows:
        issues.append(f'{len(missing_windows)} pass window(s) have no summary (rotation/capture may be incomplete)')
    if mismatches:
        issues.append(f'{len(mismatches)} window(s) have pass sums different from measured_ms (censored rows)')
    sessions = retention(observations, issues)
    timing, rejected, invalid_frames, reconciliation_failures = span_analysis(by_type, issues)
    censored_frames = sum(bool(o['row']['dropped'] or o['row'].get('reconciliation_failed') or
                               (o['stream'], o['row']['frame']) in invalid_frames) for o in by_type['frame'])
    rejected_windows = {r['source'] for r in rejected if r['type'] == 'window'}
    censored_windows = sum(bool(o['row']['dropped'] or o['row'].get('failed_slots') or o['row'].get('partial') or
                                o['source'] in rejected_windows) for o in by_type['window'])
    ordinal_mismatches = sum(
        'ordinals' in windows.get((o['stream'], o['row']['window_start'], o['row']['window_end']), {}) and
        bool(o['row']['ordinal']) != windows[(o['stream'], o['row']['window_start'], o['row']['window_end'])]['ordinals']
        for o in by_type['pass'])
    if ordinal_mismatches:
        issues.append(f'{ordinal_mismatches} pass row(s) disagree with window ordinals metadata')
    frame_ids = {}
    for observation in by_type['frame']:
        frame_ids.setdefault(observation['stream'], []).append(observation['row']['frame'])
    missing_frames = sum(b - a - 1 for values in frame_ids.values()
                         for a, b in zip(sorted(values), sorted(values)[1:]) if b > a + 1)
    pass_rows = [o['row'] for o in by_type['pass']]
    long_rows = [o['row'] for o in by_type['long_frame']]
    return {
        'schema': 'totk-gpuprof-analysis/3', 'files': list(map(str, paths)), 'sessions': sessions,
        'legacy_span_replay': legacy_span_replay, 'rejected_spans': rejected,
        'coverage': {**counts, 'streams': len({o['stream'] for o in observations}),
                     'windows': len(windows), 'pass_rows': len(pass_rows), 'frame_rows': len(by_type['frame']),
                     'long_frame_rows': len(long_rows), 'long_frames': len({(o['stream'], o['row']['frame'])
                                                                          for o in by_type['long_frame']}),
                     'missing_frame_ids': missing_frames, 'missing_window_summaries': len(missing_windows),
                     'window_sum_mismatches': len(mismatches), 'censored_frames': censored_frames,
                     'censored_windows': censored_windows, 'reconciliation_failures': reconciliation_failures,
                     'invalid_frame_spans': timing['frame']['invalid_rows'],
                     'invalid_window_spans': timing['window']['invalid_rows'],
                     'unavailable_timestamps': sum(o['row'].get('unavailable_timestamps', 0) for o in by_type['frame']),
                     'invalid_timestamps': sum(o['row'].get('invalid_timestamps', 0) for o in by_type['frame']),
                     'timestamp_wraps': sum(o['row'].get('timestamp_wraps', 0) for o in by_type['frame']),
                     'producer_invalid_spans': sum(r.get('invalid_spans', 0) for r in windows.values()),
                     'missing_windows': sum(s['missing_windows'] for s in sessions),
                     'rotation_evictions': sum(s['rotation_evictions'] for s in sessions),
                     'missing_rotation_files': sum(s['missing_rotation_files'] for s in sessions),
                     'streams_without_end': sum(not s['clean_end'] for s in sessions),
                     'header_conflicts': sum(s['header_conflict'] for s in sessions),
                     'output_failures': sum(s['output_failures'] for s in sessions),
                     'ordinal_metadata_mismatches': ordinal_mismatches,
                     'stats_missing': sum(r['count'] - r['stats_valid'] for r in pass_rows),
                     'dropped': sum(r['dropped'] for r in windows.values()),
                     'failed_slots': sum(r.get('failed_slots', 0) for r in windows.values()),
                     'stats_failed_slots': sum(r.get('stats_failed_slots', 0) for r in windows.values())},
        'timing': timing,
        'passes': aggregate(pass_rows), 'families': aggregate(pass_rows, True),
        'long_frame_passes': aggregate(long_rows), 'long_frame_families': aggregate(long_rows, True),
        'issues': issues,
        'method': 'Sum times/counters before division; ratios never averaged. Ordinal 0 means all frame ordinals '
                  '(window aggregates only); positive ordinals identify individual frame positions. Time/draw ratios '
                  'use explicitly matched statistics time/draws, or complete-stat rows when absent. Families ignore '
                  'shader/key/ordinal but keep kind, samples, scene_relative and render/attachment layout. '
                  'Scene-relative layouts use exact scene-reference ratios; other layouts use absolute dimensions. '
                  'All contributing exact keys, absolute areas and scene-reference variants remain in areas. '
                  'Long-frame rows are threshold-selected, not a complete frame population. Window/frame timing '
                  'are separate views, never added together. Orderly writer end does not prove GPU query retirement. '
                  'Spans above 10000 ms/frame are rejected even if an older producer reported reconciliation success. '
                  'Legacy replay reads frame/window timing only; it cannot reconstruct query availability, original '
                  'endpoints or schema-3 attachment/reference semantics.',
    }


def fmt(value):
    return '-' if value is None else f'{value:.3f}' if isinstance(value, float) else str(value)


def render_table(title, groups, out):
    print(f'\n## {title}', file=out)
    print('key(s) / ordinal(s) | kind | render area(s) | GPU sum ms | passes | draws | stats valid/missing | '
          'verts/draw | prims/draw | fragments/prim | ns/vertex | ns/primitive', file=out)
    for group in groups:
        ratios = group['ratios'] or {}
        render_areas = sorted({(a['render_width'], a['render_height']) for a in group['areas']})
        ordinals = ','.join('all (0)' if ordinal == 0 else str(ordinal) for ordinal in group['ordinals'])
        values = [','.join(group['keys']) + ' / ' + ordinals, group['kind'],
                  ','.join(f'{w}x{h}' for w, h in render_areas), group['gpu_ms'], group['count'], group['draws'],
                  f'{group["stats_valid"]}/{group["stats_missing"]}', *(ratios.get(k) for k in RATIOS)]
        print(' | '.join(map(fmt, values)), file=out)
        print(f'  structure: samples={group["samples"]} areas='
              f'{json.dumps(group["areas"], sort_keys=True, separators=(",", ":"))} '
              f'shaders={",".join(group["shaders"])}', file=out)
        print('  counters: ' + ('unavailable' if group['stats'] is None else
              ' '.join(f'{k}={group["stats"][k]}' for k in STATS)) +
              f'; ratio matched draws={group["ratio_draws"]} ms={group["ratio_gpu_ms"]:.3f}'
              f'; partial-stat rows={group["stats_partial_rows"]}', file=out)


def render(report, out=sys.stdout):
    print('# GPU profile schema 3', file=out)
    print(report['method'], file=out)
    print('Coverage: ' + ' '.join(f'{k}={v}' for k, v in report['coverage'].items()), file=out)
    for kind, timing in report['timing'].items():
        print(f'{kind} timing sums: ' + ' '.join(f'{k}={fmt(v)}' for k, v in timing.items()), file=out)
    for issue in report['issues']:
        print(f'WARNING: {issue}', file=out)
    render_table('Per pass (exact key; ordinal 0 = all)', report['passes'], out)
    render_table('Structural families (shader ignored)', report['families'], out)
    if report['long_frame_passes']:
        render_table('Selected long-frame passes (not window totals)', report['long_frame_passes'], out)
        render_table('Selected long-frame families', report['long_frame_families'], out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('inputs', nargs='+', help='JSONL files or quoted globs; include .jsonl.* for rotations')
    parser.add_argument('--json', metavar='FILE', help='write machine-readable analysis; - writes JSON only to stdout')
    parser.add_argument('--legacy-span-replay', action='store_true',
                        help='accept schema-2 frame/window timing only; do not reinterpret old pass areas')
    args = parser.parse_args(argv)
    try:
        report = analyze(args.inputs, args.legacy_span_replay)
        if args.json:
            text = json.dumps(report, indent=2, allow_nan=False) + '\n'
            if args.json == '-':
                sys.stdout.write(text)
            else:
                target = Path(args.json).resolve()
                guard_output(target)
                if str(target) in report['files']:
                    raise ValueError('JSON output must not overwrite an input')
                target.write_text(text, encoding='utf-8')
        if args.json != '-':
            render(report)
    except (OSError, ValueError) as error:
        parser.exit(2, f'gpuprof: {error}\n')
    return int(bool(report['coverage']['malformed_rows'] or report['coverage']['conflicting_rows']))


if __name__ == '__main__':
    sys.exit(main())
