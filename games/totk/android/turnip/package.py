#!/usr/bin/env python3
"""Package built libraries without including link-time Android platform stubs.

Usage: package.py <work-dir> <variant> <build-seconds>

Writes <work-dir>/packages/<variant>/{driver.so,meta.json}, a deterministic
adrenotools zip and a hash record under <work-dir>/logs.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import zipfile

root = Path(sys.argv[1]).resolve()
variant = sys.argv[2]
seconds = int(sys.argv[3])
out = root / 'packages' / variant
logs = root / 'logs'
logs.mkdir(parents=True, exist_ok=True)
mesa = root / 'mesa'


def git(*args):
    return subprocess.check_output(['git', '-C', str(mesa), *args], text=True).strip()


revision = git('rev-parse', 'HEAD')
tree = git('rev-parse', 'HEAD^{tree}')
dirty = bool(git('status', '--porcelain'))
# The patches are applied with `git am`, so HEAD's id is local; report the
# upstream tip recorded by build.sh so meta.json matches the reference build.
version_revision = os.environ.get('MESA_GIT_SHA1_OVERRIDE') or revision
meta = {
    'schemaVersion': 1,
    'name': f'TOTK Mesa Turnip {version_revision[:10]} {variant}',
    'description': 'Mesa 27.0.0-devel; KGSL; A6xx/A7xx/A8xx; NDK r28c; Android API 33.',
    'author': 'Mesa contributors / local TOTK build',
    'packageVersion': '1',
    'vendor': 'Mesa',
    'driverVersion': 'Mesa 27.0.0-devel',
    'minApi': 33,
    'libraryName': 'driver.so',
}
(out / 'meta.json').write_text(json.dumps(meta, indent=2) + '\n')
# Only driver.so + meta.json belong in an adrenotools archive. Symbols stay beside it.
epoch = int(os.environ['SOURCE_DATE_EPOCH'])
stamp = time.gmtime(max(epoch, 315532800))[:6]
archive = root / 'packages' / f'turnip-{variant}.zip'
with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for name in ['driver.so', 'meta.json']:
        info = zipfile.ZipInfo(name, stamp)
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o100644 << 16
        z.writestr(info, (out / name).read_bytes(), compresslevel=9)
record = {'revision': revision, 'tree': tree, 'dirty': dirty, 'build_seconds': seconds,
          'source_date_epoch': epoch, 'variant': variant, 'files': {}}
for path in [*sorted(out.iterdir()), archive]:
    if path.is_file():
        record['files'][str(path.relative_to(root))] = {
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'bytes': path.stat().st_size}
(logs / f'{variant}-package.json').write_text(json.dumps(record, indent=2) + '\n')
build = root / f'build-{variant}'
meson = os.environ.get('MESON') or shutil.which('meson')
if meson:
    options = subprocess.check_output([meson, 'introspect', '--buildoptions', str(build)], text=True)
    (logs / f'{variant}-buildoptions.json').write_text(options)
