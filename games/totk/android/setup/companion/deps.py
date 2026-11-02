"""libwebp for the on-device companion generator: fetched at the pinned 1.6.0 release (BSD-3-Clause, COPYING and PATENTS
in the source tree) and built as static libraries for the host or Android. 1.6.0 is the libwebp that Pillow 12.3.0
(requirements.txt) bundles, so the setup stage encodes the same WebP files as scripts/make_companion_assets.py.

    prepare_libwebp(work, log, target='host'|'android', ndk=<NDK dir>, api=33) -> install prefix
    (include/webp/{encode,decode,mux,demux}.h, lib/lib{webpdemux,webpmux,webp,sharpyuv}.a); setup.mk takes it as
    WEBP_PREFIX_HOST / WEBP_PREFIX_ANDROID (companion/sources.mk). The licence texts are <work>/src/libwebp/{COPYING,PATENTS}.
"""
import os
import shutil
import subprocess
from pathlib import Path

LIBWEBP_URL = 'https://github.com/webmproject/libwebp.git'
LIBWEBP_TAG = 'v1.6.0'
LIBWEBP_COMMIT = '4fa21912338357f89e4fd51cf2368325b59e9bd9'   # the tag's commit; a moved tag is refused
LIBS = ('libwebpdemux.a', 'libwebpmux.a', 'libwebp.a', 'libsharpyuv.a')   # link order


def _run(cmd, log, cwd=None):
    with open(log, 'a') as f:
        f.write('$ ' + ' '.join(map(str, cmd)) + '\n'); f.flush()
        r = subprocess.run([str(c) for c in cmd], cwd=cwd, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode:
        raise RuntimeError(f'{Path(str(cmd[0])).name} failed (exit {r.returncode}); see {log}')


def fetch_libwebp(work: Path, log: Path) -> Path:
    src = Path(work) / 'src/libwebp'
    marker = src / '.fetched-ref'
    if marker.is_file() and marker.read_text() == LIBWEBP_COMMIT:
        return src
    shutil.rmtree(src, ignore_errors=True)
    src.mkdir(parents=True)
    _run(['git', '-C', src, 'init', '-q'], log)
    _run(['git', '-C', src, 'fetch', '-q', '--depth', '1', LIBWEBP_URL, f'refs/tags/{LIBWEBP_TAG}'], log)
    _run(['git', '-C', src, 'checkout', '-q', 'FETCH_HEAD'], log)
    head = subprocess.run(['git', '-C', str(src), 'rev-parse', 'HEAD'], capture_output=True, text=True, check=True).stdout.strip()
    if head != LIBWEBP_COMMIT:
        raise RuntimeError(f'libwebp {LIBWEBP_TAG} is commit {head}, expected {LIBWEBP_COMMIT}')
    marker.write_text(LIBWEBP_COMMIT)
    return src


def prepare_libwebp(work: Path, log: Path, *, target='host', ndk=None, api=33) -> Path:
    if target not in ('host', 'android'):
        raise ValueError(f'target must be host or android, not {target!r}')
    if target == 'android' and not (ndk and (Path(ndk) / 'build/cmake/android.toolchain.cmake').is_file()):
        raise RuntimeError('prepare_libwebp(target="android") needs the NDK directory')
    work, log = Path(work), Path(log)
    log.parent.mkdir(parents=True, exist_ok=True)
    tag = 'host' if target == 'host' else f'android-arm64-{api}'
    prefix = work / f'libwebp-{tag}'
    stamp = prefix / '.built-ref'
    if stamp.is_file() and stamp.read_text() == LIBWEBP_COMMIT and all((prefix / 'lib' / l).is_file() for l in LIBS):
        return prefix
    src = fetch_libwebp(work, log)
    build = work / f'libwebp-build-{tag}'
    shutil.rmtree(build, ignore_errors=True)
    shutil.rmtree(prefix, ignore_errors=True)
    # Libraries only (encoder, decoder, mux, demux); no tools, no image readers, no threads in the encoder config we use.
    args = ['cmake', '-S', src, '-B', build, '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_INSTALL_PREFIX={prefix}',
            '-DCMAKE_INSTALL_LIBDIR=lib', '-DCMAKE_POSITION_INDEPENDENT_CODE=ON', '-DBUILD_SHARED_LIBS=OFF',
            '-DWEBP_BUILD_ANIM_UTILS=OFF', '-DWEBP_BUILD_CWEBP=OFF', '-DWEBP_BUILD_DWEBP=OFF', '-DWEBP_BUILD_GIF2WEBP=OFF',
            '-DWEBP_BUILD_IMG2WEBP=OFF', '-DWEBP_BUILD_VWEBP=OFF', '-DWEBP_BUILD_WEBPINFO=OFF', '-DWEBP_BUILD_WEBPMUX=OFF',
            '-DWEBP_BUILD_EXTRAS=OFF', '-DWEBP_BUILD_LIBWEBPMUX=ON', '-DWEBP_USE_THREAD=OFF',
            '-DCMAKE_C_FLAGS=-fvisibility=hidden']
    if target == 'android':
        args += [f'-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake', '-DANDROID_ABI=arm64-v8a',
                 f'-DANDROID_PLATFORM=android-{api}']
    _run(args, log)
    _run(['cmake', '--build', build, '--target', 'install', '-j', str(os.cpu_count() or 4)], log)
    if not all((prefix / 'lib' / l).is_file() for l in LIBS) or not (prefix / 'include/webp/encode.h').is_file():
        raise RuntimeError(f'libwebp install incomplete in {prefix}; see {log}')
    stamp.write_text(LIBWEBP_COMMIT)
    return prefix


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser(description='Fetch and build the pinned libwebp for the companion generator.')
    ap.add_argument('work', type=Path)
    ap.add_argument('--target', choices=('host', 'android'), default='host')
    ap.add_argument('--ndk', type=Path)
    ap.add_argument('--api', type=int, default=33)
    a = ap.parse_args()
    print(prepare_libwebp(a.work, a.work / f'libwebp-{a.target}.log', target=a.target, ndk=a.ndk, api=a.api))
