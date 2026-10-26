#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Summarize always-on walker CPU windows; compare runs without equating frame index with route position."""
import argparse
from dataclasses import dataclass
from itertools import combinations
from pathlib import Path
import re
import statistics
import sys

NUMBER = r"(-?\d+(?:\.\d+)?)"
CPU = re.compile(r"\[nvn-vk\] walker cpu frame (\d+): cpu " + NUMBER +
                 r" ms/frame, wall " + NUMBER + r" ms/frame, draws " + NUMBER +
                 r"/frame, cpu " + NUMBER + r" us/draw, offcpu " + NUMBER + r" ms/frame")
FPS = re.compile(r"\[nvn-vk\] frame (\d+): " + NUMBER + r" fps over the last 300")
BINS = re.compile(r"interval bins.*?all=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+);")


@dataclass(frozen=True)
class Window:
    frame: int
    cpu: float
    wall: float
    draws: float
    per_draw: float
    offcpu: float

    @property
    def key(self):
        return self.frame - 299, self.frame


@dataclass
class Run:
    path: str
    windows: list
    fps: list
    long_frames: int
    intervals: int
    bin_windows: int

    @property
    def per_draw(self):
        draws = sum(w.draws for w in self.windows)
        # The logged ratio retains more precision than rounded ms/frame when draw counts are small.
        return sum(w.per_draw * w.draws for w in self.windows) / draws if draws else None


def load(path, start):
    windows, fps = [], []
    counts = [0, 0, 0]
    last_frame = 0
    long_frames = intervals = bin_windows = 0
    with Path(path).open(errors="replace") as log:
        for line in log:
            match = CPU.search(line)
            if match:
                counts[0] += 1
                w = Window(int(match[1]), *(float(match[i]) for i in range(2, 7)))
                if w.frame <= last_frame or w.frame % 300:
                    raise ValueError(f"{path}: duplicate, restarted or non-300 frame window {w.frame}")
                last_frame = w.frame
                if min(w.cpu, w.wall, w.draws, w.per_draw) < 0 or w.cpu > w.wall + 0.002 or w.offcpu < -0.002:
                    raise ValueError(f"{path}: invalid CPU/raw-wall values at frame {w.frame}")
                if abs(w.wall - w.cpu - w.offcpu) > 0.003:
                    raise ValueError(f"{path}: inconsistent offcpu at frame {w.frame}")
                if counts[0] >= start:
                    windows.append(w)
            elif "[nvn-vk] walker cpu frame " in line:
                raise ValueError(f"{path}: malformed walker CPU line")
            match = FPS.search(line)
            if match:
                counts[1] += 1
                if counts[1] >= start:
                    fps.append(float(match[2]))
            match = BINS.search(line)
            if match:
                counts[2] += 1
                bins = [int(x) for x in match.groups()]
                # Exactly thor-route-env.sh: NR>=9 && $4>=150; long=$5+$6, n=sum(all six).
                if counts[2] >= start and bins[3] >= 150:
                    bin_windows += 1
                    intervals += sum(bins)
                    long_frames += bins[4] + bins[5]
    if not windows:
        raise ValueError(f"{path}: no walker CPU windows from #{start} (found {counts[0]}); use the metric APK and a long enough run")
    return Run(path, windows, fps, long_frames, intervals, bin_windows)


def value(x, digits=3):
    return f"{x:.{digits}f}" if x is not None else "n/a"


def summarize(run):
    windows = run.windows
    print(f"{run.path}: cpu {statistics.mean(w.cpu for w in windows):.3f} ms/frame, "
          f"cpu {value(run.per_draw)} us/draw, wall {statistics.mean(w.wall for w in windows):.3f} ms/frame, "
          f"draws {statistics.mean(w.draws for w in windows):.3f}/frame, "
          f"fps {value(statistics.mean(run.fps) if run.fps else None, 2)}, "
          f"long {value(100 * run.long_frames / run.intervals if run.intervals else None, 2)}% "
          f"(cpu {len(windows)} win, fps {len(run.fps)} win, long {run.bin_windows} win/{run.intervals} intervals)")


def percentile(values, p):
    ordered = sorted(values)
    at = (len(ordered) - 1) * p
    lo = int(at)
    return ordered[lo] + (ordered[min(lo + 1, len(ordered) - 1)] - ordered[lo]) * (at - lo)


def pair(left, right):
    a = {w.key: w for w in left.windows if w.draws}
    b = {w.key: w for w in right.windows if w.draws}
    keys = sorted(a.keys() & b.keys())
    if not keys:
        raise ValueError("no common nonzero-draw 300-frame ranges")
    delta = [b[k].per_draw - a[k].per_draw for k in keys]
    draw_change = [100 * (b[k].draws / a[k].draws - 1) for k in keys]
    print(f"exploratory frame-index pairing B-A: {len(keys)} pairs, unmatched A/B {len(a)-len(keys)}/{len(b)-len(keys)}, "
          f"median {statistics.median(delta):+.3f} us/draw, IQR [{percentile(delta, .25):+.3f}, {percentile(delta, .75):+.3f}], "
          f"range [{min(delta):+.3f}, {max(delta):+.3f}]; median draws change {statistics.median(draw_change):+.2f}%")
    print("Frame ranges: " + ", ".join(f"{lo}-{hi}" for lo, hi in keys))
    print("Exploratory only: equal frame indices do not establish matching route positions, scenes or workloads. "
          "No independence, confidence, causality or 0.3-us/draw detection claim follows.")


def calibrate(runs):
    ratios = [r.per_draw for r in runs]
    if any(x is None for x in ratios):
        raise ValueError("calibration requires nonzero draw totals in every log")
    differences = [abs(b - a) for a, b in combinations(ratios, 2)]
    print(f"baseline-vs-baseline empirical spread: {len(ratios)} runs, "
          f"median {statistics.median(ratios):.3f} us/draw, sample SD {statistics.stdev(ratios):.3f}, "
          f"range [{min(ratios):.3f}, {max(ratios):.3f}]; "
          f"absolute run-pair difference median {statistics.median(differences):.3f}, "
          f"p95 {percentile(differences, .95):.3f}, max {max(differences):.3f} us/draw")
    print("Use independent unchanged-baseline runs under the same route, clocks, caches and window rules. "
          "Pairwise differences share runs and are not independent samples. Their empirical p95/max help calibrate "
          "a practical noise threshold; they are not a confidence bound or a guaranteed detection limit.")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--pair", action="store_true", help="exploratory equal-frame-range comparison of exactly two logs; not route matching")
    mode.add_argument("--calibrate", action="store_true", help="empirical run-to-run spread from at least three unchanged-baseline logs")
    parser.add_argument("--start-window", type=int, default=9, help="first retained window in each log stream (default: 9)")
    parser.add_argument("logs", nargs="+")
    args = parser.parse_args(argv)
    if args.start_window < 1:
        parser.error("--start-window must be positive")
    if args.pair and len(args.logs) != 2:
        parser.error("--pair needs exactly two logs")
    if args.calibrate and len(args.logs) < 3:
        parser.error("--calibrate needs at least three independent baseline logs")
    try:
        runs = [load(path, args.start_window) for path in args.logs]
        for run in runs:
            summarize(run)
        for run in runs[1:]:
            if run.per_draw is not None and runs[0].per_draw is not None:
                print(f"run difference {run.path} - {runs[0].path}: {run.per_draw - runs[0].per_draw:+.3f} us/draw (draw-weighted whole-run ratio)")
        if args.pair:
            pair(*runs)
        elif args.calibrate:
            calibrate(runs)
    except (OSError, ValueError) as error:
        print(f"walker-summary: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
