"""Capstone for the on-device rewriter: fetched at the pinned 5.0.9 release (the version requirements.txt pins for the
Python rewriter, so both disassemble identically) and built as a static, arm64-only library for the host or Android.

    prepare_capstone(work, log, target='host'|'android', ndk=<NDK dir>, api=33) -> install prefix
    (include/capstone/capstone.h, lib/libcapstone.a); setup.mk takes it as CAPSTONE_PREFIX_HOST / CAPSTONE_PREFIX_ANDROID.
"""
import os
import shutil
import subprocess
from pathlib import Path

CAPSTONE_URL = 'https://github.com/capstone-engine/capstone.git'
CAPSTONE_TAG = '5.0.9'
CAPSTONE_COMMIT = '022575848782a4801fd150fdbc927effcbca0864'   # the tag's commit; a moved tag is refused


def _run(cmd, log, cwd=None):
    with open(log, 'a') as f:
        f.write('$ ' + ' '.join(map(str, cmd)) + '\n'); f.flush()
        r = subprocess.run([str(c) for c in cmd], cwd=cwd, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode:
        raise RuntimeError(f'{Path(str(cmd[0])).name} failed (exit {r.returncode}); see {log}')


def fetch_capstone(work: Path, log: Path) -> Path:
    src = Path(work) / 'src/capstone'
    marker = src / '.fetched-ref'
    if marker.is_file() and marker.read_text() == CAPSTONE_COMMIT:
        return src
    shutil.rmtree(src, ignore_errors=True)
    src.mkdir(parents=True)
    _run(['git', '-C', src, 'init', '-q'], log)
    _run(['git', '-C', src, 'fetch', '-q', '--depth', '1', CAPSTONE_URL, f'refs/tags/{CAPSTONE_TAG}'], log)
    _run(['git', '-C', src, 'checkout', '-q', 'FETCH_HEAD'], log)
    head = subprocess.run(['git', '-C', str(src), 'rev-parse', 'HEAD'], capture_output=True, text=True, check=True).stdout.strip()
    if head != CAPSTONE_COMMIT:
        raise RuntimeError(f'capstone {CAPSTONE_TAG} is commit {head}, expected {CAPSTONE_COMMIT}')
    marker.write_text(CAPSTONE_COMMIT)
    return src


def prepare_capstone(work: Path, log: Path, *, target='host', ndk=None, api=33) -> Path:
    if target not in ('host', 'android'):
        raise ValueError(f'target must be host or android, not {target!r}')
    if target == 'android' and not (ndk and (Path(ndk) / 'build/cmake/android.toolchain.cmake').is_file()):
        raise RuntimeError('prepare_capstone(target="android") needs the NDK directory')
    work, log = Path(work), Path(log)
    log.parent.mkdir(parents=True, exist_ok=True)
    tag = 'host' if target == 'host' else f'android-arm64-{api}'
    prefix = work / f'capstone-{tag}'
    stamp = prefix / '.built-ref'
    if stamp.is_file() and stamp.read_text() == CAPSTONE_COMMIT and (prefix / 'lib/libcapstone.a').is_file():
        return prefix
    src = fetch_capstone(work, log)
    build = work / f'capstone-build-{tag}'
    shutil.rmtree(build, ignore_errors=True)
    shutil.rmtree(prefix, ignore_errors=True)
    args = ['cmake', '-S', src, '-B', build, '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_INSTALL_PREFIX={prefix}',
            '-DCMAKE_INSTALL_LIBDIR=lib', '-DCMAKE_POSITION_INDEPENDENT_CODE=ON', '-DBUILD_SHARED_LIBS=OFF',
            '-DBUILD_STATIC_LIBS=ON', '-DCAPSTONE_ARCHITECTURE_DEFAULT=OFF', '-DCAPSTONE_ARM64_SUPPORT=ON',
            '-DCAPSTONE_BUILD_TESTS=OFF', '-DCAPSTONE_BUILD_CSTOOL=OFF', '-DCAPSTONE_BUILD_CSTEST=OFF',
            '-DCAPSTONE_INSTALL=ON']
    if target == 'android':
        args += [f'-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake', '-DANDROID_ABI=arm64-v8a',
                 f'-DANDROID_PLATFORM=android-{api}']
    _run(args, log)
    _run(['cmake', '--build', build, '--target', 'install', '-j', str(os.cpu_count() or 4)], log)
    if not (prefix / 'lib/libcapstone.a').is_file() or not (prefix / 'include/capstone/capstone.h').is_file():
        raise RuntimeError(f'capstone install incomplete in {prefix}; see {log}')
    stamp.write_text(CAPSTONE_COMMIT)
    return prefix


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser(description='Fetch and build the pinned capstone for the rewriter.')
    ap.add_argument('work', type=Path)
    ap.add_argument('--target', choices=('host', 'android'), default='host')
    ap.add_argument('--ndk', type=Path)
    ap.add_argument('--api', type=int, default=33)
    a = ap.parse_args()
    print(prepare_capstone(a.work, a.work / f'capstone-{a.target}.log', target=a.target, ndk=a.ndk, api=a.api))
