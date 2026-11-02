"""Zstandard for the on-device shader seed: Tears of the Kingdom's RomFS files are zstd frames, most of them compressed
with the dictionaries of Pack/ZsDic.pack.zs. Fetched at the pinned upstream release (BSD licence, LICENSE in the source
tree) and built as a static, decompression-only library for the host or Android.

    prepare_zstd(work, log, target='host'|'android', ndk=<NDK dir>, api=33) -> install prefix
    (include/zstd.h, lib/libzstd.a); setup.mk takes it as ZSTD_PREFIX_HOST / ZSTD_PREFIX_ANDROID (seed/sources.mk).
    The licence text is <work>/src/zstd/LICENSE.
"""
import os
import shutil
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ZSTD_URL = 'https://github.com/facebook/zstd.git'
ZSTD_TAG = 'v1.5.7'
ZSTD_COMMIT = 'f8745da6ff1ad1e7bab384bd1f9d742439278e99'   # the tag's commit; a moved tag is refused
# Decompression and the frame/dictionary helpers it needs; no compressor, dictionary builder or legacy formats.
SOURCES = ('common/debug.c', 'common/entropy_common.c', 'common/error_private.c', 'common/fse_decompress.c',
           'common/pool.c', 'common/threading.c', 'common/xxhash.c', 'common/zstd_common.c',
           'decompress/huf_decompress.c', 'decompress/zstd_ddict.c', 'decompress/zstd_decompress.c',
           'decompress/zstd_decompress_block.c')
# ZSTD_DISABLE_ASM: the x86-64 Huffman assembly is the only .S file; the C path is the one Android (arm64) uses anyway.
# XXH_NAMESPACE as zstd's own build sets it, so a second xxhash in the same process cannot clash.
CFLAGS = ['-O2', '-fPIC', '-fvisibility=hidden', '-DZSTD_DISABLE_ASM', '-DZSTD_LEGACY_SUPPORT=0', '-DXXH_NAMESPACE=ZSTD_']


def _run(cmd, log, cwd=None):
    with open(log, 'a') as f:
        f.write('$ ' + ' '.join(map(str, cmd)) + '\n'); f.flush()
        r = subprocess.run([str(c) for c in cmd], cwd=cwd, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode:
        raise RuntimeError(f'{Path(str(cmd[0])).name} failed (exit {r.returncode}); see {log}')


def fetch_zstd(work: Path, log: Path) -> Path:
    src = Path(work) / 'src/zstd'
    marker = src / '.fetched-ref'
    if marker.is_file() and marker.read_text() == ZSTD_COMMIT:
        return src
    shutil.rmtree(src, ignore_errors=True)
    src.mkdir(parents=True)
    _run(['git', '-C', src, 'init', '-q'], log)
    _run(['git', '-C', src, 'fetch', '-q', '--depth', '1', ZSTD_URL, f'refs/tags/{ZSTD_TAG}'], log)
    _run(['git', '-C', src, 'checkout', '-q', 'FETCH_HEAD'], log)
    head = subprocess.run(['git', '-C', str(src), 'rev-parse', 'HEAD'], capture_output=True, text=True, check=True).stdout.strip()
    if head != ZSTD_COMMIT:
        raise RuntimeError(f'zstd {ZSTD_TAG} is commit {head}, expected {ZSTD_COMMIT}')
    marker.write_text(ZSTD_COMMIT)
    return src


def _toolchain(target, ndk, api):
    if target == 'host':
        return [os.environ.get('HOST_CC', 'cc')], os.environ.get('HOST_AR', 'ar')
    host = 'darwin-x86_64' if os.uname().sysname == 'Darwin' else 'linux-x86_64'
    bin_ = Path(ndk) / f'toolchains/llvm/prebuilt/{host}/bin'
    return [bin_ / 'clang', f'--target=aarch64-linux-android{api}'], bin_ / 'llvm-ar'


def prepare_zstd(work: Path, log: Path, *, target='host', ndk=None, api=33) -> Path:
    if target not in ('host', 'android'):
        raise ValueError(f'target must be host or android, not {target!r}')
    if target == 'android' and not (ndk and (Path(ndk) / 'toolchains/llvm/prebuilt').is_dir()):
        raise RuntimeError('prepare_zstd(target="android") needs the NDK directory')
    work, log = Path(work), Path(log)
    log.parent.mkdir(parents=True, exist_ok=True)
    tag = 'host' if target == 'host' else f'android-arm64-{api}'
    prefix = work / f'zstd-{tag}'
    stamp = prefix / '.built-ref'
    if stamp.is_file() and stamp.read_text() == ZSTD_COMMIT and (prefix / 'lib/libzstd.a').is_file():
        return prefix
    src = fetch_zstd(work, log)
    cc, ar = _toolchain(target, ndk, api)
    build = work / f'zstd-build-{tag}'
    shutil.rmtree(build, ignore_errors=True)
    shutil.rmtree(prefix, ignore_errors=True)
    build.mkdir(parents=True)
    objects = [build / (s.replace('/', '_')[:-2] + '.o') for s in SOURCES]
    with ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        for future in [pool.submit(_run, [*cc, *CFLAGS, '-c', src / 'lib' / s, '-o', o], log) for s, o in zip(SOURCES, objects)]:
            future.result()
    (prefix / 'lib').mkdir(parents=True)
    (prefix / 'include').mkdir()
    _run([ar, 'rcs', prefix / 'lib/libzstd.a', *objects], log)
    for header in ('zstd.h', 'zstd_errors.h', 'zdict.h'):
        shutil.copy2(src / 'lib' / header, prefix / 'include' / header)
    stamp.write_text(ZSTD_COMMIT)
    return prefix


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser(description='Fetch and build the pinned zstd (decompression only) for the shader seed.')
    ap.add_argument('work', type=Path)
    ap.add_argument('--target', choices=('host', 'android'), default='host')
    ap.add_argument('--ndk', type=Path)
    ap.add_argument('--api', type=int, default=33)
    a = ap.parse_args()
    print(prepare_zstd(a.work, a.work / f'zstd-{a.target}.log', target=a.target, ndk=a.ndk, api=a.api))
