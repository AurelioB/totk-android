#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Analyze a saved [pso-life] log; never infer lifetime from address reuse alone.

The report describes descriptor MAY-reads and recorded pass-end/non-render writes
in one bounded recording-frame window, not shader texel fetches or pixel validity.
The first window frame is a boundary exclusion for transient candidacy; an
initial read there is still unknown, never evidence of established history.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys


MARKER = '[pso-life]'
READ_ONLY_UNKNOWN_REASONS = frozenset({
    'texture-to-buffer-read', 'texture-copy-read', 'writeback-read',
    'native-image-read', 'diagnostic-readback', 'cpu-texel-read',
    'presenter-read', 'image-upgrade-read',
})
IDENTITY_FIELDS = ('image', 'incarnation', 'mip', 'layer', 'aspect')
IMAGE_FIELDS = ('frame', 'sub', 'pass', *IDENTITY_FIELDS, 'w', 'h', 'format',
                'serial', 'storage')
REQUIRED = {
    'begin': ('w', 'h', 'limit'),
    'write': (*IMAGE_FIELDS, 'cleared', 'draws'),
    'read': (*IMAGE_FIELDS, 'stage', 'binding', 'access', 'shader',
             'consumer_w', 'consumer_h'),
    'frame-end': ('frame', 'reads', 'writes', 'unknown'),
    'end': ('frame', 'complete', 'unknown'),
    'unknown': ('reason',),
}
INTEGER_FIELDS = set(IMAGE_FIELDS + ('start', 'limit', 'cleared', 'draws', 'stage',
    'binding', 'consumer_w', 'consumer_h', 'reads', 'writes', 'unknown', 'complete'))
INTEGER_FIELDS -= {'image', 'storage'}
POSITIVE_FIELDS = {'frame', 'incarnation', 'aspect', 'w', 'h', 'start', 'limit'}
LIMITATIONS = [
    'Descriptor reads are MAY-read bindings, not proof of shader texel fetches; '
    'conservative false-history results are possible.',
    'Exactly accounted read-only extra-consumer unknowns preserve writer-version '
    'coverage only. Every unknown still prevents complete consumer coverage and '
    'transient certification.',
    'Writes include load/inline clears, accepted render writes and source-tagged non-render writes. Only '
    'cleared=1 establishes a full base; uploads/copies can preserve prior pixels. '
    'Storage-write descriptors are MAY-writes, not established overwrites.',
    'A non-cleared write can preserve a LOAD base. A current-frame writer alone '
    'does not prove that prior-frame pixels were overwritten.',
    'No native packed-pixel validation, source-byte inspection, GPU copy or '
    'rendered-output equivalence is performed.',
    'Verdicts cover only the selected actual mip extent and observed recording '
    'frames. Recurrence, alternating buffers, serials and storage addresses alone '
    'do not prove transient or history lifetime.',
    'Transient candidacy excludes the first recording frame and is not authority '
    'to relax classification, half/quarter sizing or behavior outside this window.',
]


def parse_record(raw, line):
    """Parse one marked line, rejecting partial/duplicate fields rather than guessing."""
    payload = raw.split(MARKER, 1)[1].strip()
    fields = {}
    for token in payload.split():
        key, separator, value = token.partition('=')
        if (not separator or not value or not re.fullmatch(r'[a-z][a-z0-9_]*', key)
                or key in fields):
            raise ValueError(f'invalid or duplicate key=value token: {token!r}')
        fields[key] = value
    kind = fields.get('kind')
    if kind not in REQUIRED:
        raise ValueError(f'unknown or missing kind: {kind!r}')
    missing = [key for key in REQUIRED[kind] if key not in fields]
    if missing:
        raise ValueError('missing fields: ' + ', '.join(missing))
    for key in sorted(INTEGER_FIELDS & fields.keys()):
        if not re.fullmatch(r'[0-9]+', fields[key]):
            raise ValueError(f'{key} is not a nonnegative decimal integer')
        fields[key] = int(fields[key])
        if key in POSITIVE_FIELDS and fields[key] == 0:
            raise ValueError(f'{key} must be positive')
    for key in ('image', 'storage'):
        if key in fields:
            if not re.fullmatch(r'0[xX][0-9a-fA-F]+', fields[key]):
                raise ValueError(f'{key} is not a hexadecimal address')
            fields[key] = hex(int(fields[key], 16))
    if 'image' in fields and fields['image'] == '0x0':
        raise ValueError('image must identify a non-null lifetime')
    for key in ('cleared', 'complete'):
        if key in fields and fields[key] not in (0, 1):
            raise ValueError(f'{key} must be 0 or 1')
    if kind == 'read' and fields['access'] not in ('sampled', 'storage-read', 'storage-write'):
        raise ValueError('unsupported descriptor access')
    if kind == 'write' and not (fields['draws'] or fields['cleared'] or fields.get('source')):
        raise ValueError('write has neither successful draws, a full clear nor a non-render source')
    if kind == 'begin':
        fields.setdefault('start', 1)
    return dict(fields, line=line, raw=raw)


def identity_key(record):
    return tuple(record[key] for key in IDENTITY_FIELDS)


def identity_name(key):
    return ':'.join(str(value) for value in key)


def identity_sort(key):
    return (int(key[0], 16), *key[1:])


def missing_ranges(present, first, last):
    """Represent missing frame ranges without allocating up to an untrusted limit."""
    gaps, next_frame = [], first
    for frame in sorted(frame for frame in present if first <= frame <= last):
        if frame > next_frame:
            gaps.append(dict(first=next_frame, last=frame - 1))
        next_frame = frame + 1
    if next_frame <= last:
        gaps.append(dict(first=next_frame, last=last))
    return gaps


def analyze(text):
    """Return deterministic JSON-compatible evidence, including malformed-line anchors."""
    records, issues = [], []

    def issue(code, record=None, **details):
        item = dict(code=code, **details)
        if record is not None:
            item.update(line=record['line'], raw=record['raw'])
            if 'frame' in record:
                item['frame'] = record['frame']
        issues.append(item)

    for line, raw in enumerate(text.splitlines(), 1):
        if MARKER not in raw:
            continue
        try:
            records.append(parse_record(raw, line))
        except ValueError as error:
            issue('malformed_record', dict(line=line, raw=raw), message=str(error))

    frames, begins, ends, unknowns = {}, [], [], []
    active, ended, last_frame = False, False, 0
    structural_valid = not issues
    for record in records:
        kind = record['kind']
        if kind == 'begin':
            if begins or active or ended:
                issue('multiple_begin_records', record)
                structural_valid = False
            begins.append(record)
            active = True
            continue
        if not active or ended:
            issue('record_outside_begin_end', record)
            structural_valid = False
        if kind == 'end':
            ends.append(record)
            if len(ends) > 1:
                issue('multiple_end_records', record)
                structural_valid = False
            ended = True
            continue
        if kind == 'unknown':
            unknowns.append(record)
            issue('runtime_unknown', record, reason=record['reason'])
            if 'frame' not in record:
                structural_valid = False
                continue
        frame = record['frame']
        if frame < last_frame:
            issue('frame_order_regression', record, previous_frame=last_frame)
            structural_valid = False
        last_frame = max(frame, last_frame)
        bucket = frames.setdefault(frame, dict(frame=frame, writes=[], reads=[],
                                               unknowns=[], frame_ends=[]))
        if bucket['frame_ends']:
            issue('record_after_frame_end', record)
            structural_valid = False
        if kind == 'frame-end':
            bucket['frame_ends'].append(record)
        else:
            bucket[{'write': 'writes', 'read': 'reads', 'unknown': 'unknowns'}[kind]].append(record)
        if begins and kind in ('write', 'read'):
            begin = begins[0]
            if (record['w'], record['h']) != (begin['w'], begin['h']):
                issue('extent_outside_selected_filter', record)
                structural_valid = False

    begin = begins[0] if begins else None
    end = ends[0] if ends else None
    if begin is None:
        issue('missing_begin')
        structural_valid = False
    if end is None:
        issue('missing_end')
    elif not end['complete']:
        issue('end_incomplete', end)
    if end and end['unknown']:
        issue('end_reports_unknown', end, count=end['unknown'])
    first_frame = begin['start'] if begin else 1
    limit = first_frame + begin['limit'] - 1 if begin else (end['frame'] if end else last_frame)
    if begin and end and end['frame'] != limit:
        issue('end_frame_does_not_match_window', end, expected_last_frame=limit)
    for frame, bucket in sorted(frames.items()):
        reasons = []
        if len(bucket['frame_ends']) != 1:
            reasons.append('missing_or_duplicate_frame_end')
        else:
            coverage = bucket['frame_ends'][0]
            for key in ('reads', 'writes'):
                if coverage[key] != len(bucket[key]):
                    reasons.append(f'{key}_count_mismatch')
            if coverage['unknown']:
                reasons.append('frame_reports_unknown')
        if bucket['unknowns']:
            reasons.append('runtime_unknown')
        if not first_frame <= frame <= limit:
            reasons.append('frame_outside_window')
        unknown_rows_accounted = (len(bucket['frame_ends']) == 1
            and bucket['frame_ends'][0]['unknown'] == len(bucket['unknowns']))
        if len(bucket['frame_ends']) == 1 and not unknown_rows_accounted:
            reasons.append('unknown_count_mismatch')
        readonly_unknown_rows = sum(record['reason'] in READ_ONLY_UNKNOWN_REASONS
                                    for record in bucket['unknowns'])
        writer_reasons = list(reasons)
        if unknown_rows_accounted and readonly_unknown_rows == len(bucket['unknowns']):
            writer_reasons = [reason for reason in writer_reasons
                              if reason not in ('runtime_unknown', 'frame_reports_unknown')]
        bucket['coverage'] = dict(complete=not reasons,
            observed_reads=len(bucket['reads']), observed_writes=len(bucket['writes']),
            reasons=reasons, writer_complete=not writer_reasons,
            writer_reasons=writer_reasons, unknown_rows_accounted=unknown_rows_accounted,
            readonly_unknown_rows=readonly_unknown_rows)
        if reasons:
            issue('incomplete_frame', frame=frame, reasons=reasons,
                  lines=[record['line'] for record in bucket['frame_ends']])
    gaps = missing_ranges({frame for frame, bucket in frames.items()
                           if len(bucket['frame_ends']) == 1}, first_frame, limit)
    if gaps:
        issue('missing_frame_ends', ranges=gaps)
    reported_unknown = sum(record['unknown'] for record in records
                           if record['kind'] == 'frame-end')
    # An unlocalized end counter cannot establish where writer coverage was lost.
    if end and end['unknown'] > reported_unknown:
        structural_valid = False
        issue('unlocalized_end_unknown', end)
    complete = bool(begin and end and end['complete'] and not issues)

    run_starts, previous = {}, None
    for frame, bucket in sorted(frames.items()):
        if bucket['coverage']['writer_complete']:
            run_starts[frame] = (run_starts[previous] if previous == frame - 1
                                 and previous in run_starts else frame)
        previous = frame

    identities, states, identity_records = {}, {}, {}
    old_reads, initial_reads, boundary_reads, load_caveats = [], [], [], []
    for record in records:
        if record['kind'] not in ('write', 'read'):
            continue
        key = identity_key(record)
        name = identity_name(key)
        record['identity'] = name
        identity_records.setdefault(key, []).append(record)
        identity = identities.setdefault(key, dict(id=name,
            **dict(zip(IDENTITY_FIELDS, key)), first_line=record['line'],
            last_line=record['line'], write_lines=[], read_lines=[], write_frames=[],
            write_recurrence=[], serial_changes=[], storage_changes=[],
            serials=[], storage_addresses=[], formats=[], extents=[]))
        state = states.setdefault(key, dict(writer=None, base_frame=None,
                                           base_line=None, storage_write_line=None))
        identity['last_line'] = record['line']
        for field, values, changes in (('serial', 'serials', 'serial_changes'),
                                        ('storage', 'storage_addresses', 'storage_changes')):
            if not identity[values] or identity[values][-1] != record[field]:
                if identity[values]:
                    identity[changes].append(dict(line=record['line'], frame=record['frame'],
                                                   previous=identity[values][-1], value=record[field]))
                identity[values].append(record[field])
        if record['format'] not in identity['formats']:
            identity['formats'].append(record['format'])
        extent = dict(w=record['w'], h=record['h'])
        if extent not in identity['extents']:
            identity['extents'].append(extent)
        if record['kind'] == 'write':
            writer = state['writer']
            identity['write_lines'].append(record['line'])
            if not identity['write_frames'] or identity['write_frames'][-1] != record['frame']:
                if writer is not None:
                    elapsed = record['frame'] - writer['frame']
                    identity['write_recurrence'].append(dict(previous_frame=writer['frame'],
                        frame=record['frame'], elapsed_frames=elapsed,
                        gap_frames=max(0, elapsed - 1), previous_line=writer['line'],
                        line=record['line']))
                identity['write_frames'].append(record['frame'])
            if record['cleared']:
                state['base_frame'], state['base_line'] = record['frame'], record['line']
            record.update(base_frame=state['base_frame'], base_write_line=state['base_line'],
                          base_established_current_frame=state['base_frame'] == record['frame'])
            if not record['cleared']:
                load_caveats.append(dict(line=record['line'], frame=record['frame'],
                    identity=name, base_frame=state['base_frame'],
                    base_write_line=state['base_line'],
                    base_established_current_frame=record['base_established_current_frame']))
            state['writer'] = record
            state['storage_write_line'] = None
            continue
        identity['read_lines'].append(record['line'])
        writer = state['writer']
        writer_frame = writer['frame'] if writer else None
        covered = bool(structural_valid and writer and record['frame'] in run_starts
                       and run_starts[record['frame']] <= writer_frame <= record['frame'])
        record.update(previous_writer_frame=writer_frame,
            previous_writer_line=writer['line'] if writer else None,
            age_frames=record['frame'] - writer_frame if writer else None,
            writer_full_clear=bool(writer['cleared']) if writer else None,
            base_frame=state['base_frame'], base_write_line=state['base_line'],
            base_established_current_frame=state['base_frame'] == record['frame'],
            writer_coverage_established=covered,
            read_before_current_write=writer_frame != record['frame'],
            established_prior_frame=False,
            load_preserved_base_caveat=bool(writer and not writer['cleared']))
        if record['access'] == 'storage-write':
            record['classification'] = 'storage_write_descriptor'
            state['storage_write_line'] = record['line']
        elif writer is None:
            record['classification'] = ('first_frame_boundary_unknown' if record['frame'] == first_frame
                                         and begin else 'unknown_initial_read')
            (boundary_reads if record['classification'] == 'first_frame_boundary_unknown'
             else initial_reads).append(record)
        elif state['storage_write_line'] is not None:
            record['classification'] = 'unknown_intervening_storage_write'
        elif writer_frame == record['frame']:
            record['classification'] = 'current_frame_producer'
        elif writer_frame < record['frame'] and covered:
            record['classification'] = 'prior_frame_read'
            record['established_prior_frame'] = True
            old_reads.append(record)
        else:
            record['classification'] = 'prior_writer_unverified_coverage'

    groups = {}
    addresses = {}
    for key, identity in sorted(identities.items(), key=lambda item: identity_sort(item[0])):
        identity['write_gaps'] = [gap for gap in identity['write_recurrence'] if gap['gap_frames']]
        addresses.setdefault(key[0], set()).add(key[1])
        for record in identity_records[key]:
            group_key = (record['w'], record['h'], record['format'])
            group = groups.setdefault(group_key, dict(w=group_key[0], h=group_key[1],
                format=group_key[2], identities=set(), roots=set(), image_addresses=set(),
                write_roots_by_frame={}))
            group['identities'].add(identity['id'])
            root = f'{key[0]}:{key[1]}'
            group['roots'].add(root)
            group['image_addresses'].add(key[0])
            if record['kind'] == 'write':
                group['write_roots_by_frame'].setdefault(record['frame'], set()).add(root)
    group_reports = []
    for _, group in sorted(groups.items()):
        write_roots = sorted(group.pop('write_roots_by_frame').items())
        alternating = (len(group['roots']) == 2 and len(write_roots) >= 3
            and all(len(roots) == 1 for _, roots in write_roots)
            and all(after == before + 1 and before_roots != after_roots
                    for (before, before_roots), (after, after_roots)
                    in zip(write_roots, write_roots[1:])))
        for field in ('identities', 'roots', 'image_addresses'):
            group[field] = sorted(group[field])
        group.update(identity_count=len(group['identities']), root_count=len(group['roots']),
                     image_address_count=len(group['image_addresses']),
                     alternating_two_root_writes=alternating,
                     recurrence_is_lifetime_proof=False)
        group_reports.append(group)

    blockers = []
    if not complete:
        blockers.append('incomplete_or_unknown_trace_authority')
    if limit <= first_frame:
        blockers.append('no_post_boundary_frame')
    for frame, bucket in sorted(frames.items()):
        if frame == first_frame:
            continue
        content_reads = [record for record in bucket['reads'] if record['access'] != 'storage-write']
        if not bucket['writes'] or not content_reads:
            blockers.append(f'frame_{frame}_lacks_write_and_content_read_evidence')
        if any(not record['base_established_current_frame'] for record in bucket['writes']):
            blockers.append(f'frame_{frame}_write_has_unknown_or_preserved_base')
        if any(record['classification'] != 'current_frame_producer'
               or not record['base_established_current_frame']
               or not record['writer_coverage_established'] for record in content_reads):
            blockers.append(f'frame_{frame}_read_lacks_full_current_frame_producer')
        if any(record['access'] == 'storage-write' and not record['base_established_current_frame']
               for record in bucket['reads']):
            blockers.append(f'frame_{frame}_storage_write_lacks_full_current_frame_base')
    for key, observations in sorted(identity_records.items(), key=lambda item: identity_sort(item[0])):
        evaluated = [record for record in observations if record['frame'] > first_frame]
        if (not any(record['kind'] == 'write' for record in evaluated)
                or not any(record['kind'] == 'read' and record['access'] != 'storage-write'
                           for record in evaluated)):
            blockers.append(f'identity_{identity_name(key)}_lacks_post_boundary_write_read_evidence')
    if not identities:
        blockers.append('no_matching_image_evidence')
    verdict = ('history_or_persistent' if old_reads else 'unknown' if blockers
               else 'transient_candidate')
    return dict(schema_version=1, verdict=verdict,
        scope=dict(selected_extent=dict(w=begin['w'], h=begin['h']) if begin else None,
                   first_recording_frame=first_frame, requested_last_frame=limit,
                   requested_frames=begin['limit'] if begin else None,
                   transient_candidate_first_frame=first_frame + 1,
                   first_frame_boundary_excluded=True,
                   scope='observed bounded recording-frame window only'),
        trace=dict(begin=begin, end=end, complete=complete,
                   valid_records=len(records), malformed_records=sum(
                       item['code'] == 'malformed_record' for item in issues),
                   unknown_records=unknowns, reported_frame_unknown_total=reported_unknown,
                   missing_frame_end_ranges=gaps, issues=issues),
        summary=dict(identity_count=len(identities), root_count=len({key[:2] for key in identities}),
                     image_address_count=len(addresses),
                     established_prior_frame_reads=len(old_reads),
                     unknown_initial_reads=len(initial_reads),
                     first_frame_boundary_unknown_reads=len(boundary_reads),
                     load_preserved_base_writes=len(load_caveats),
                     transient_candidate_blockers=blockers),
        established_prior_frame_reads=old_reads, unknown_initial_reads=initial_reads,
        first_frame_boundary_unknown_reads=boundary_reads, load_base_caveats=load_caveats,
        frames=[bucket for _, bucket in sorted(frames.items())],
        identities=[identity for _, identity in sorted(identities.items(),
                                                     key=lambda item: identity_sort(item[0]))],
        image_address_reuse=[dict(image=image, incarnations=sorted(incarnations))
                             for image, incarnations in sorted(addresses.items(),
                                 key=lambda item: int(item[0], 16)) if len(incarnations) > 1],
        groups=group_reports, limitations=LIMITATIONS)




def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path, help='saved text log containing [pso-life] records')
    parser.add_argument('--output', type=Path, help='write JSON here instead of stdout')
    args = parser.parse_args(argv)
    try:
        if args.output and args.output.resolve() == args.log.resolve():
            raise ValueError('--output must not overwrite the input log')
        contents = args.log.read_bytes()
        report = analyze(contents.decode('utf-8', errors='replace'))
        report['source'] = dict(path=str(args.log.resolve()), bytes=len(contents),
                                sha256=hashlib.sha256(contents).hexdigest())
        output = json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + '\n'
        if args.output:
            args.output.write_text(output, encoding='utf-8')
        else:
            sys.stdout.write(output)
    except (OSError, ValueError) as error:
        print(f'target-lifetime: {error}', file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
