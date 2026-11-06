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
RATIOS = ('verts_per_draw', 'prims_per_draw', 'fragments_per_prim', 'ns_per_vertex', 'ns_per_primitive')


def number(row, key, integer=False, minimum=0):
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
        for key in ('span_ms', 'measured_ms', 'gap_ms'):
            number(row, key)
        number(row, 'dropped', True)
        for key in ('failed_slots', 'stats_failed_slots', 'frames'):
            if key in row:
                number(row, key, True)
        for key in ('partial', 'reconciliation_failed'):
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
        number(row, 'ordinal', True)
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
    if kind == 'window':
        fields = ('window_start', 'window_end')
    elif kind == 'pass':
        fields = ('window_start', 'window_end', 'key', 'ordinal', *REFERENCE)
    elif kind == 'frame':
        fields = ('frame',)
    else:
        fields = ('frame', 'key', 'ordinal', *REFERENCE)
    return (stream, kind, *(row[key] for key in fields))


def load(paths):
    def reject_constant(text):
        raise ValueError(f'nonfinite JSON number {text}')

    observations, conflicts = {}, set()
    issues = []
    counts = dict(malformed_rows=0, truncated_rows=0, duplicate_rows=0, conflicting_rows=0, blank_lines=0)
    for path in paths:
        stream = stream_path(path)
        with path.open(encoding='utf-8', errors='replace') as file:
            for line_no, line in enumerate(file, 1):
                if not line.strip():
                    counts['blank_lines'] += 1
                    continue
                where = f'{path}:{line_no}'
                try:
                    row = validate(json.loads(line, parse_constant=reject_constant))
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
            if row['stats_valid'] == row['count']:
                group['ratio_draws'] += row['draws']
                group['ratio_gpu_ms'] += row['gpu_ms']
                for key in STATS:
                    group['ratio_stats'][key] += row['stats'][key]
            else:
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


def analyze(paths):
    paths = inputs(paths)
    observations, counts, issues = load(paths)
    by_type = {kind: [] for kind in ('window', 'pass', 'frame', 'long_frame')}
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
    mismatches = [ident for ident, row in windows.items()
                  if not math.isclose(pass_sums.get(ident, 0.0), row['measured_ms'], abs_tol=0.01, rel_tol=1e-5)]
    if missing_windows:
        issues.append(f'{len(missing_windows)} pass window(s) have no summary (rotation/capture may be incomplete)')
    if mismatches:
        issues.append(f'{len(mismatches)} window(s) have pass sums different from measured_ms (censored rows)')
    censored_frames = sum(bool(o['row']['dropped'] or o['row'].get('reconciliation_failed'))
                          for o in by_type['frame'])
    censored_windows = sum(bool(row['dropped'] or row.get('failed_slots') or row.get('partial'))
                           for row in windows.values())
    reconciliation_failures = 0
    for kind in ('window', 'frame'):
        for observation in by_type[kind]:
            row = observation['row']
            if not math.isclose(row['span_ms'], row['measured_ms'] + row['gap_ms'], abs_tol=0.01, rel_tol=1e-5):
                reconciliation_failures += 1
                issues.append(f'{observation["source"]}: span != measured + gap')
    frame_ids = {}
    for observation in by_type['frame']:
        frame_ids.setdefault(observation['stream'], []).append(observation['row']['frame'])
    missing_frames = sum(b - a - 1 for values in frame_ids.values()
                         for a, b in zip(sorted(values), sorted(values)[1:]) if b > a + 1)
    pass_rows = [o['row'] for o in by_type['pass']]
    long_rows = [o['row'] for o in by_type['long_frame']]
    return {
        'schema': 'totk-gpuprof-analysis/3', 'files': list(map(str, paths)),
        'coverage': {**counts, 'streams': len({o['stream'] for o in observations}),
                     'windows': len(windows), 'pass_rows': len(pass_rows), 'frame_rows': len(by_type['frame']),
                     'long_frame_rows': len(long_rows), 'long_frames': len({(o['stream'], o['row']['frame'])
                                                                          for o in by_type['long_frame']}),
                     'missing_frame_ids': missing_frames, 'missing_window_summaries': len(missing_windows),
                     'window_sum_mismatches': len(mismatches), 'censored_frames': censored_frames,
                     'censored_windows': censored_windows, 'reconciliation_failures': reconciliation_failures,
                     'stats_missing': sum(r['count'] - r['stats_valid'] for r in pass_rows),
                     'dropped': sum(r['dropped'] for r in windows.values()),
                     'failed_slots': sum(r.get('failed_slots', 0) for r in windows.values()),
                     'stats_failed_slots': sum(r.get('stats_failed_slots', 0) for r in windows.values())},
        'timing': {kind: {field: sum(o['row'][field] for o in by_type[kind])
                          for field in ('span_ms', 'measured_ms', 'gap_ms')} for kind in ('window', 'frame')},
        'passes': aggregate(pass_rows), 'families': aggregate(pass_rows, True),
        'long_frame_passes': aggregate(long_rows), 'long_frame_families': aggregate(long_rows, True),
        'issues': issues,
        'method': 'Sum times/counters before division; ratios never averaged. Time/draw ratios use only rows with '
                  'complete statistics; partial-stat counters remain in counter-to-counter ratios. Families ignore '
                  'shader/key/ordinal but keep kind, samples, scene_relative and render/attachment layout. '
                  'Scene-relative layouts use exact scene-reference ratios; other layouts use absolute dimensions. '
                  'All contributing exact keys, absolute areas and scene-reference variants remain in areas. '
                  'Long-frame rows are threshold-selected, not a complete frame population. Window/frame timing '
                  'are separate views, never added together. Missing leading/trailing captures cannot be inferred.',
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
        values = [','.join(group['keys']) + ' / ' + ','.join(map(str, group['ordinals'])), group['kind'],
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
        print(f'{kind} timing sums: ' + ' '.join(f'{k}={v:.3f}' for k, v in timing.items()), file=out)
    for issue in report['issues']:
        print(f'WARNING: {issue}', file=out)
    render_table('Per pass (exact key + ordinal)', report['passes'], out)
    render_table('Structural families (shader ignored)', report['families'], out)
    if report['long_frame_passes']:
        render_table('Selected long-frame passes (not window totals)', report['long_frame_passes'], out)
        render_table('Selected long-frame families', report['long_frame_families'], out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('inputs', nargs='+', help='JSONL files or quoted globs; include .jsonl.* for rotations')
    parser.add_argument('--json', metavar='FILE', help='write machine-readable analysis; - writes JSON only to stdout')
    args = parser.parse_args(argv)
    try:
        report = analyze(args.inputs)
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
