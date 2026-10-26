#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise walker summary window selection, weighting, pairing and baseline noise calibration."""
import argparse
import contextlib
import io
from pathlib import Path
import walker_summary as summary


def metric(frame, cpu, draws):
    return (f"[nvn-vk] walker cpu frame {frame}: cpu {cpu:.3f} ms/frame, wall {cpu+2:.3f} ms/frame, "
            f"draws {draws:.3f}/frame, cpu {cpu*1000/draws:.3f} us/draw, offcpu 2.000 ms/frame\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scratch', type=Path, required=True)
    scratch = parser.parse_args().scratch.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    logs = []
    for run, scale in enumerate((1, 1.02, .98)):
        text = ''
        for i in range(1, 13):
            cpu = 999 if i < 9 else (i - 8) * 10 * scale
            draws = 1000 if i % 2 else 2000
            text += metric(i * 300, cpu, draws)
            text += f'[nvn-vk] frame {i*300}: {999 if i < 9 else i+21}.0 fps over the last 300, 0 draws\n'
            bins = '10/20/30/150/40/50' if i == 10 else '0/0/0/200/10/20' if i == 11 else '0/0/0/149/10/20'
            text += f'[nvn-vk] window present-fence interval bins ms [...]: all={bins}; adjacent immutable interval2=0/0/0/0/0/0\n'
        path = scratch / f'summary-baseline-{run}.log'
        path.write_text(text)
        logs.append(str(path))
    run = summary.load(logs[0], 9)
    assert len(run.windows) == 4 and run.windows[0].key == (2401, 2700)
    assert run.per_draw is not None and abs(run.per_draw - 100000 / 6000) < 1e-9
    assert run.fps == [30, 31, 32, 33]
    assert run.bin_windows == 2 and run.long_frames == 120 and run.intervals == 530
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        assert summary.main(logs) == 0
        assert summary.main(['--pair'] + logs[:2]) == 0
        assert summary.main(['--calibrate'] + logs) == 0
    text = output.getvalue()
    assert 'cpu 25.000 ms/frame, cpu 16.667 us/draw' in text
    assert 'long 22.64%' in text and 'fps 31.50' in text
    assert '+0.333 us/draw' in text and '4 pairs' in text
    assert 'Exploratory only' in text and 'not a confidence bound' in text
    # Missing optional streams are explicit; partial CPU logs fail closed under the default warmup rule.
    short = scratch / 'summary-short.log'
    short.write_text(metric(300, 10, 1000))
    output = io.StringIO()
    with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
        assert summary.main([str(short)]) == 2
        assert summary.main(['--start-window', '1', str(short)]) == 0
    assert 'fps n/a, long n/a%' in output.getvalue()
    # Equal-frame matching reports unmatched windows, without pretending they are route positions.
    other = scratch / 'summary-unmatched.log'
    other.write_text(metric(2700, 10, 1000) + metric(3000, 20, 2000))
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        summary.pair(run, summary.load(other, 1))
    assert '2 pairs, unmatched A/B 2/0' in output.getvalue()
    for name, text in [('duplicate', metric(300, 1, 1000) * 2),
                       ('bad-clock', metric(300, 1, 1000).replace('wall 3.000', 'wall 0.000')),
                       ('malformed', '[nvn-vk] walker cpu frame 300: broken\n')]:
        bad = scratch / f'summary-{name}.log'
        bad.write_text(text)
        try:
            summary.load(bad, 1)
        except ValueError:
            pass
        else:
            raise AssertionError(f'accepted {name}')
    print('PASS: ninth-window rules, ratio weighting, legacy long bins, exploratory pairing, calibration, absent streams and malformed/restarted logs')


if __name__ == '__main__':
    main()
