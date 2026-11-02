"""The on-device shader seed stage (android/setup/seed) against its Python references.

SyntheticSeed (needs cc, git and Python 3.14's compression.zstd; no game data): a RomFS made here with the game's
container shapes (Zstandard with a trained dictionary pack, SARC, BNSH, agl .sharcb, a shader embedded in an ordinary
file, one in a game module and one the shared list locates) must give the inventory and plan of
scripts/prepare_shader_seed.py exactly; malformed shared lists are rejected as scripts/shader_list.py rejects them; with a
translator (see below) the one real compute program in it is translated and seed.vkpack round-trips with
scripts/pack_shader_seed.py.

SeedOracle (skipped without the reference work folder TOTK_REF_WORK, default
build/TOTK_build/totk in the workspace folder that holds this checkout, and a translator): the full inventory and plan on the real
1.4.3 RomFS and rewritten modules equal prepare_shader_seed.py's for the repository's shader list; then a deterministic
sample of shader groups (every TOTK_SEED_SAMPLE-th, default 40; 1 = everything) is translated with the PC seed's shader
list and compared variant by variant with the PC seed (TOTK_PC_SEED, default build/TOTK_android/shaders/v143, made by
build_android.py --shaders: Metal service + --keep-spirv + vk_reflect.py).

Translator: TOTK_SEED_TRANSLATOR, else prebuilt/linux-x64/totk_xlate when it has --vulkan, else a host NativeAOT publish
(tools/totk_xlate/publish_native.sh, needs the .NET 10 SDK) cached in TOTK_SEED_WORK (default games/totk/build/setup-seed).
Outputs go to temporary folders; nothing is written into the reference work folder."""
import hashlib
import importlib.util
import json
import os
import random
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GAME = ROOT / 'games/totk'
SETUP = GAME / 'android/setup'
SEED = SETUP / 'seed'
SCRIPTS = GAME / 'scripts'
WORK = Path(os.environ.get('TOTK_SEED_WORK', GAME / 'build/setup-seed'))
REF = Path(os.environ.get('TOTK_REF_WORK', ROOT.parent / 'build/TOTK_build/totk'))
PC_SEED = Path(os.environ.get('TOTK_PC_SEED', ROOT.parent / 'build/TOTK_android/shaders/v143'))
SAMPLE = int(os.environ.get('TOTK_SEED_SAMPLE', '40'))
sys.path.insert(0, str(SCRIPTS))
import pack_shader_seed  # noqa: E402

try:
    import compression.zstd as zstd   # Python 3.14+
except ImportError:
    zstd = None


def _module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


_CLI = {}


def seed_cli():
    """android/setup/seed/seed_cli.c with the seed sources and the core helpers they use, warnings as errors."""
    if 'cli' not in _CLI:
        WORK.mkdir(parents=True, exist_ok=True)
        prefix = _module('seed_deps', SEED / 'deps.py').prepare_zstd(WORK, WORK / 'zstd-host.log', target='host')
        out = WORK / 'seed_cli'
        srcs = sorted(SEED.glob('*.c')) + [SETUP / n for n in ('util.c', 'json.c', 'sha256.c', 'romfs_image.c')]
        subprocess.run(['cc', '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-I', SETUP, '-I', SEED,
                        '-I', prefix / 'include', *srcs, prefix / 'lib/libzstd.a', '-lpthread', '-o', out], check=True)
        _CLI['cli'] = out
    return _CLI['cli']


def has_vulkan(binary):
    if not binary or not Path(binary).is_file():
        return False
    with tempfile.TemporaryDirectory() as d:
        r = subprocess.run([binary, '--service', '--vulkan', '--out', d, '--chain', 'probe'], input=b'', capture_output=True)
    return r.returncode == 0


def translator():
    """A host translator with the Vulkan service, or None."""
    if 'xlate' in _CLI:
        return _CLI['xlate']
    found = None
    for candidate in (os.environ.get('TOTK_SEED_TRANSLATOR'), GAME / 'prebuilt/linux-x64/totk_xlate', WORK / 'xlate-host/totk_xlate'):
        if candidate and has_vulkan(candidate):
            found = Path(candidate); break
    dotnet = os.environ.get('DOTNET') or shutil.which('dotnet')
    if not found and dotnet and sys.platform.startswith('linux'):
        out = WORK / 'xlate-host'
        r = subprocess.run(['sh', GAME / 'tools/totk_xlate/publish_native.sh', '--dotnet', dotnet, '--out', out],
                           cwd=GAME, capture_output=True)
        if r.returncode == 0 and has_vulkan(out / 'totk_xlate'):
            found = out / 'totk_xlate'
    _CLI['xlate'] = found
    return found


def libdir(tmp):
    """<libdir>/libtotk_xlate.so as the APK installs it."""
    d = Path(tmp) / 'lib'
    d.mkdir(exist_ok=True)
    link = d / 'libtotk_xlate.so'
    if not link.exists():
        link.symlink_to(translator() or '/nonexistent')
    return d


def run_cli(romfs, modules, out, lib, *extra):
    return subprocess.run([seed_cli(), '--romfs', romfs, '--rewritten', modules, '--out', out, '--libdir', lib,
                           '--staging', Path(out).parent / 'staging', *map(str, extra)], capture_output=True, text=True)


def read_dump(path):
    shaders, jobs = {}, []
    for line in Path(path).read_text().splitlines():
        f = line.split('\t')
        if f[0] == 'S':
            shaders[f[1]] = dict(stage=int(f[2]), startup=int(f[3]), located=int(f[4]), control=int(f[5]), code=int(f[6]))
        else:
            jobs.append((f[1], f[2]))
    return shaders, jobs


def python_plan(romfs, modules, shader_list):
    """prepare_shader_seed.py's inventory, located() and plan(), the way prepare() calls them; jobs regrouped per shader
    in first-job order (prepare's by_key), the order the C stage translates and dumps them."""
    import prepare_shader_seed as p
    from shader_list import ShaderList
    _, shaders, startup, _ = p.inventory(Path(romfs), Path(modules))
    shared = ShaderList.load(shader_list)
    extra = p.located({w for w in shared.locators if w[0] not in shaders}, p.source_reader(Path(romfs), Path(modules)), 'totk')
    shaders.update(extra)
    groups = {}
    for key, _, variant, why in p.plan(shaders, startup, shared.variants):
        groups.setdefault(key, []).append((variant, why))
    return shaders, startup, set(extra), [j for g in groups.values() for j in g]


def assert_same_plan(test, dump, shaders, startup, located, jobs):
    c_shaders, c_jobs = read_dump(dump)
    test.assertEqual(set(c_shaders), set(shaders))
    stage = {'vs': 0, 'ps': 1, 'cs': 5}
    for key, (st, control, code, _) in shaders.items():
        test.assertEqual((c_shaders[key]['stage'], c_shaders[key]['control'], c_shaders[key]['code']),
                         (stage[st], len(control), len(code)), key)
    test.assertEqual({k for k, v in c_shaders.items() if v['startup']}, set(startup))
    test.assertEqual({k for k, v in c_shaders.items() if v['located']}, located)
    test.assertEqual(c_jobs, jobs)


def read_pack(path):
    """{name: bytes} of a TOTKVKP1 file, checking the layout pack_shader_seed.py writes."""
    data = Path(path).read_bytes()
    magic, version, record, count, size = pack_shader_seed.HEADER.unpack_from(data, 0)
    assert (magic, version, record, size) == (pack_shader_seed.MAGIC, 1, pack_shader_seed.RECORD.size, len(data))
    files, names, end = {}, [], pack_shader_seed.HEADER.size + count * record
    for i in range(count):
        raw, reserved, offset, length = pack_shader_seed.RECORD.unpack_from(data, pack_shader_seed.HEADER.size + i * record)
        name = raw.rstrip(b'\0').decode('ascii')
        assert reserved == 0 and offset % 8 == 0 and offset == (end + 7) & ~7 and not any(data[end:offset])
        files[name] = data[offset:offset + length]; names.append(raw); end = offset + length
    assert names == sorted(names) and end == len(data)
    return files


def seed_id(files):
    """optimize_shader_seed.py's content identity of a seed."""
    h = hashlib.sha256()
    for name in sorted(files):
        h.update(name.encode() + b'\0' + hashlib.sha256(files[name]).digest())
    return h.hexdigest()


def assert_pack_round_trip(test, out):
    files = read_pack(out / 'seed.vkpack')
    test.assertEqual(sorted(p.name for p in out.iterdir()), ['seed-id.txt', 'seed.vkpack'])
    test.assertEqual((out / 'seed-id.txt').read_text(), seed_id(files) + '\n')
    with tempfile.TemporaryDirectory() as d:   # the Python packer makes the same bytes from the same loose files
        for name, payload in files.items():
            (Path(d) / name).write_bytes(payload)
        test.assertEqual(pack_shader_seed.pack_seed(d, Path(d) / 'again.vkpack').read_bytes(), (out / 'seed.vkpack').read_bytes())
    return files


# ---- synthetic game files ----
def sarc(members):
    names, table, data = bytearray(), [], bytearray()
    for name, payload in members:
        while len(data) % 8:
            data.append(0)
        table.append((len(names) // 4, len(data), len(data) + len(payload)))
        names += name.encode() + b'\0'
        while len(names) % 4:
            names.append(0)
        data += payload
    count = len(members)
    sfat = struct.pack('<4sHHI', b'SFAT', 12, count, 0x65) + b''.join(
        struct.pack('<IIII', 0, 0x01000000 | n, lo, hi) for n, lo, hi in table)
    sfnt = struct.pack('<4sHH', b'SFNT', 8, 0) + bytes(names)
    start = (20 + len(sfat) + len(sfnt) + 7) & ~7
    head = struct.pack('<4sHHII', b'SARC', 20, 0xfeff, start + len(data), start) + b'\x00\x01\x00\x00'
    return (head + sfat + sfnt).ljust(start, b'\0') + bytes(data)


def control(stage, length, c1=0, early=0, local=(8, 8, 1), size=0x880, filler=0):
    c = bytearray(random.Random(filler).randbytes(size))
    struct.pack_into('<I', c, 0x20, size)
    struct.pack_into('<II', c, 0x700, c1, length)
    struct.pack_into('<I', c, 0x714, stage)
    c[0x718] = early
    struct.pack_into('<III', c, 0x748, *local)
    return bytes(c)


def bnsh(programs):
    """programs: [{stage name: (control, code)}]; layout as prepare_shader_seed.bnsh_shaders reads it."""
    body = bytearray(136)
    body[0:8] = b'BNSH\0\0\0\0'; body[12:14] = b'\xff\xfe'; body[96:100] = b'grsc'
    variations = len(body); body += bytes(64 * len(programs))
    struct.pack_into('<IQ', body, 124, len(programs), variations)
    for i, prog in enumerate(programs):
        at = len(body); body += bytes(56)
        struct.pack_into('<Q', body, variations + i * 64 + 16, at)
        for stage, rel in (('vs', 8), ('ps', 40), ('cs', 48)):
            if stage not in prog:
                continue
            ctl, code = prog[stage]
            rec = len(body); body += bytes(32)
            ctl_at = len(body); body += ctl
            code_at = len(body); body += code
            struct.pack_into('<QQII', body, rec + 8, ctl_at, code_at, len(code), len(ctl))
            struct.pack_into('<Q', body, at + rel, rec)
    struct.pack_into('<I', body, 28, len(body))
    return bytes(body)


def sharcb(entries):
    """agl package: records <Q code offset, I control size, i control offset from the record> before the code pool."""
    controls = bytearray(); recs = []
    pool = bytearray()
    for ctl, code in entries:
        recs.append((len(pool), len(controls)))
        controls += ctl; pool += code
    start = 32
    rec_size = 16 * len(entries)
    pool_at = start + rec_size + len(controls)
    out = bytearray(b'BAHS' + bytes(16) + struct.pack('<3I', start, len(pool), pool_at))
    for i, (code_off, ctl_off) in enumerate(recs):
        record = start + 16 * i
        out += struct.pack('<QIi', code_off, 0x880, start + rec_size + ctl_off - record)
    return bytes(out + controls + pool)


EXIT_CS = struct.pack('<4Q', 0, 0xe30000000007000f, 0, 0)   # a compute program that only exits (test_vkshader_xlate.c)


def fixture(root):
    """RomFS (romfs.bin, runtime-index.bin, index.json), modules and a shared list. Returns (romfs, modules, list, keys)."""
    r = random.Random(7)
    code = lambda n: r.randbytes(n)
    samples = [r.randbytes(64) * 20 + b'shader' * 40 for _ in range(300)]
    dictionary = zstd.train_dict(samples, 2048)
    progs = [{'vs': (control(0, 0x200, 0x60, filler=1), code(0x200)), 'ps': (control(1, 0x180, 0x40, early=1, filler=2), code(0x180))},
             {'cs': (control(5, len(EXIT_CS), 0x10, local=(64, 1, 1), filler=3), EXIT_CS)}]
    shader_pack = sarc([('a.bfsha', b'FSHA' + bytes(60) + bnsh(progs)),
                        ('agl/x.sharcb', sharcb([(control(1, 0x100, 0x20, filler=4), code(0x100))]))])
    sdk = bnsh([{'vs': (control(0, 0x80, filler=5), code(0x80))},
                {'vs': (bytes(0x400), code(0x40))}])   # the SDK's older, short control layout: skipped
    effect = r.randbytes(5000) + bnsh([{'ps': (control(1, 0x90, filler=6), code(0x90))}]) + r.randbytes(3000)
    hidden_ctl, hidden_code = control(1, 0x70, filler=8), code(0x70)
    hidden = r.randbytes(0x100) + hidden_ctl + hidden_code   # no BNSH: only the list's locator finds it
    files = {
        'Pack/ZsDic.pack.zs': zstd.compress(sarc([('zs.zsdic', dictionary.dict_content)])),
        'Shader/Main.Nin_NX_NVN.pack.zs': zstd.compress(shader_pack, zstd_dict=dictionary),
        'Lib/NintendoSDK/TextureShader.bnsh': sdk,
        'Effect/Fx.esetb.zs': zstd.compress(effect),
        'Other/hidden.bin': hidden,
        'Other/plain.txt': b'nothing here\n' * 100,
    }
    romfs = Path(root) / 'romfs'; romfs.mkdir()
    image, rows, strings, entries = bytearray(b'RomFS image'), bytearray(), bytearray(), []
    for name in sorted(files):
        while len(image) % 16:
            image.append(0)
        entries.append(dict(path=name, offset=len(image), size=len(files[name])))
        nm = name.encode() + b'\0'
        rows += struct.pack('<QQII', len(image), len(files[name]), len(strings), len(nm)); strings += nm
        image += files[name]
    (romfs / 'romfs.bin').write_bytes(image)
    (romfs / 'runtime-index.bin').write_bytes(struct.pack('<8sQQ', b'TOTKRFS1', len(files), 24 + len(rows)) + rows + strings)
    (romfs / 'index.json').write_text(json.dumps(dict(status='pass', files=entries, romfs_sha256=hashlib.sha256(image).hexdigest())))
    modules = Path(root) / 'rewritten'; modules.mkdir()
    (modules / 'main.patched.bin').write_bytes(r.randbytes(4096) + bnsh([{'cs': (control(5, len(EXIT_CS), local=(1, 1, 1), filler=9), EXIT_CS)}]))
    (modules / 'sdk.patched.bin').write_bytes(r.randbytes(4096))
    key = lambda ctl, c: hashlib.sha256(ctl + c).hexdigest()
    keys = dict(vs=key(progs[0]['vs'][0], progs[0]['vs'][1]), cs=key(progs[1]['cs'][0], EXIT_CS),
                hidden=key(hidden_ctl, hidden_code))
    lst = Path(root) / 'shader_list.tsv'
    rows = ['# TOTK shader list v1', '',
            f'V\t{keys["vs"]}\t{{"attr":{{"0":"uint","1":"float"}}}}',
            f'V\t{keys["vs"]}\t{{"attr":{{"0":"sint"}},"tex":{{"10":"2d","2":"cube"}}}}',
            f'V\t{keys["cs"]}\t{{"img":{{"0":{{"fmt":"R32Uint","type":"2d"}}}}}}',
            f'V\t{keys["cs"]}\t{{}}',
            f'V\t{keys["hidden"]}\t{{"fout":{{"0":"uint"}}}}',
            f'V\t{"ab" * 32}\t{{"tex":{{"0":"2d"}}}}',   # a shader of another game version: not planned
            f'L\t{keys["hidden"]}\tOther/hidden.bin\t100\t{0x100 + 0x880:x}\t70',
            f'L\t{"cd" * 32}\tShader/Main.Nin_NX_NVN.pack.zs/a.bfsha\t0\t0\t10',
            f'S\t{keys["vs"]}-0123456789ab_b\t0\tt', f'C\t{keys["cs"]}-0123456789ab_b\tm', 'R\t00ff\ttm']
    lst.write_text('\n'.join(rows) + '\n')
    return romfs, modules, lst, keys


@unittest.skipUnless(zstd and shutil.which('cc') and shutil.which('git'), 'needs Python 3.14 (compression.zstd), cc and git')
class SyntheticSeed(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='totk-seed-'))
        cls.romfs, cls.modules, cls.list, cls.keys = fixture(cls.tmp)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def plan(self, shader_list, name):
        out = self.tmp / name
        dump = self.tmp / f'{name}.tsv'
        lib = libdir(self.tmp) if translator() else self.tmp / 'fake-lib'
        if not translator():   # the stage checks the translator first; planning does not run it
            lib.mkdir(exist_ok=True); (lib / 'libtotk_xlate.so').write_text('#!/bin/sh\n'); (lib / 'libtotk_xlate.so').chmod(0o755)
        return run_cli(self.romfs, self.modules, out, lib, '--shader-list', shader_list, '--plan-out', dump, '--plan-only'), dump

    def test_inventory_and_plan_match_python(self):
        r, dump = self.plan(self.list, 'plan')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        shaders, startup, located, jobs = python_plan(self.romfs, self.modules, self.list)
        self.assertEqual(located, {self.keys['hidden']})
        self.assertIn(self.keys['cs'], startup)
        assert_same_plan(self, dump, shaders, startup, located, jobs)

    def test_parts_give_the_same_plan(self):
        # The same image as romfs.bin.000 ... (uneven last part, seams inside files) must give the very same inventory.
        parts = self.tmp / 'parts-romfs'; parts.mkdir()
        image = (self.romfs / 'romfs.bin').read_bytes(); size = len(image) // 4 + 3
        for i in range(0, len(image), size):
            (parts / f'romfs.bin.{i // size:03d}').write_bytes(image[i:i + size])
        self.assertGreater(len(image) % size, 0)
        shutil.copy(self.romfs / 'runtime-index.bin', parts / 'runtime-index.bin')
        r, one = self.plan(self.list, 'plan-one')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        lib = libdir(self.tmp) if translator() else self.tmp / 'fake-lib'
        dump = self.tmp / 'plan-parts.tsv'
        r = run_cli(parts, self.modules, self.tmp / 'plan-parts', lib, '--shader-list', self.list, '--plan-out', dump, '--plan-only')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(dump.read_text(), one.read_text())

    def test_files_out_lists_the_shader_files(self):
        """--full-scan --files-out writes shader_files.c: the files a full scan found shaders in, with the index's SHA-256.
        The fixture's index is not the pinned one, so its own runs scan every file (the shipped list does not apply)."""
        lib = libdir(self.tmp) if translator() else self.tmp / 'fake-lib'
        out = self.tmp / 'files.c'
        r = run_cli(self.romfs, self.modules, self.tmp / 'plan-files', lib, '--shader-list', self.list, '--plan-only', '--files-out', out)
        self.assertNotEqual(r.returncode, 0)   # a list made from a partial scan would be wrong
        r = run_cli(self.romfs, self.modules, self.tmp / 'plan-files', lib, '--shader-list', self.list, '--plan-only',
                    '--full-scan', '--files-out', out)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        text = out.read_text()
        index = hashlib.sha256((self.romfs / 'runtime-index.bin').read_bytes()).hexdigest()
        self.assertIn(f'SEED_SHADER_FILES_INDEX_SHA256[65] = "{index}";', text)
        self.assertEqual(re.findall(r'^    "(.*)",$', text, re.M),
                         ['Effect/Fx.esetb.zs', 'Lib/NintendoSDK/TextureShader.bnsh', 'Shader/Main.Nin_NX_NVN.pack.zs'])
        self.assertIn('SEED_SHADER_FILES_COUNT = 3;', text)

    def test_malformed_lists_are_rejected_like_shader_list_py(self):
        from shader_list import ShaderList
        bad = ['V\tnot-a-key\t{}', f'V\t{"ab" * 32}\t{{"tex": {{}}}}', f'V\t{"ab" * 32}\t{{"tex":{{"1":"a","0":"b"}}}}',
               f'V\t{"ab" * 32}\t{{"other":{{}}}}', f'L\t{"ab" * 32}\t../x\t0\t0\t1', f'L\t{"ab" * 32}\tA/B\t0\t0\t12345678901',
               f'S\t{"ab" * 32}-0123456789ab_b\t2\tt', 'R\t0f0\tt', f'C\t{"ab" * 32}-0123456789ab_b\tx']
        for i, row in enumerate(bad):
            lst = self.tmp / f'bad{i}.tsv'
            lst.write_text(row + '\n')
            with self.assertRaises(ValueError, msg=row):
                ShaderList.load(lst)
            r, _ = self.plan(lst, f'bad{i}')
            self.assertEqual(r.returncode, 1, row)
            self.assertIn('not a shader list row', r.stdout, row)

    @unittest.skipUnless(translator(), 'no host translator with --vulkan')
    def test_translate_and_pack(self):
        out = self.tmp / 'seed'
        r = run_cli(self.romfs, self.modules, out, libdir(self.tmp), '--shader-list', self.list)
        # Only the exit-only compute programs translate (the random-code shaders are logged as failed translations), far
        # fewer startup shaders than a playable seed needs.
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn('first-launch warm-up', r.stdout)
        self.assertFalse(out.joinpath('seed.vkpack').exists())
        r = run_cli(self.romfs, self.modules, out, libdir(self.tmp), '--shader-list', self.list, '--any-count')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        files = assert_pack_round_trip(self, out)
        # The listed image state is translated, gives the default state's deps (the program queries no image) and is
        # dropped as a duplicate; the listed {} state is the default state itself. One variant stays: the default one.
        default = self.keys['cs'] + '-' + hashlib.sha256(b'{"attr":{},"fout":{},"img":{},"tex":{}}').hexdigest()[:12]
        self.assertEqual(sorted(n for n in files if n.startswith(self.keys['cs'])),
                         [default + '.spv', default + '.vk', self.keys['cs'] + '.vv'])
        self.assertEqual(files[self.keys['cs'] + '.vv'].decode().split('\t')[0], default)
        self.assertEqual(sum(n.endswith('.spv') for n in files), 2)   # and the game module's compute program

    @unittest.skipUnless(translator(), 'no host translator with --vulkan')
    def test_scratch_in_work_dir_resumes(self):
        """--work-dir plays the app's noBackupDir: the spill and the loose files go to <W>/setup-work/shaders-<id>, never to
        the output folder. A failed run keeps that scratch and the next run with the same inputs resumes it (the shaders it
        journaled are not translated again) to the same pack; success removes it, and other runs' scratch goes too."""
        nb, out = self.tmp / 'nb', self.tmp / 'seed-nb'
        stale = nb / 'setup-work/shaders-0123456789abcdef/loose'; stale.mkdir(parents=True)
        r = run_cli(self.romfs, self.modules, out, libdir(self.tmp), '--shader-list', self.list, '--work-dir', nb)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)   # too few startup shaders: failed, scratch kept
        self.assertFalse(stale.exists())
        kept = list((nb / 'setup-work').iterdir())
        self.assertEqual(len(kept), 1)
        self.assertGreater(len((kept[0] / 'translated.journal').read_text().splitlines()), 0)
        self.assertEqual(sorted(p.name for p in out.iterdir()), [])
        r = run_cli(self.romfs, self.modules, out, libdir(self.tmp), '--shader-list', self.list, '--any-count', '--work-dir', nb)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        log = (self.tmp / 'staging' / 'shaders.log').read_text()
        self.assertRegex(log, r'resumed_shaders [1-9]')
        self.assertEqual(list((nb / 'setup-work').iterdir()), [])
        files = assert_pack_round_trip(self, out)
        plain = self.tmp / 'seed-plain'
        r = run_cli(self.romfs, self.modules, plain, libdir(self.tmp), '--shader-list', self.list, '--any-count')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual((plain / 'seed.vkpack').read_bytes(), (out / 'seed.vkpack').read_bytes())
        self.assertEqual(sorted(files), sorted(read_pack(plain / 'seed.vkpack')))


# A stand-in translator service for the watchdog tests: answers like the --vulkan service (a minimal SPIR-V module with
# the requested execution model, a .vk and its .vv row) unless <dir>/mode says to hang on a variant ("hang <variant>...")
# or on every request ("hang-all"). Each start appends its pid to <dir>/pids; each hang appends the variant to
# <dir>/hanging.
FAKE_SERVICE = r'''#!{python}
import json, os, struct, sys, time
HERE = {here!r}
out = sys.argv[sys.argv.index('--out') + 1]
mode = open(os.path.join(HERE, 'mode')).read().split()
with open(os.path.join(HERE, 'pids'), 'a') as f:
    f.write(f'{{os.getpid()}}\n')
for line in sys.stdin:
    rq = json.loads(line)
    variant = rq['variant']
    if mode[0] == 'hang-all' or (mode[0] == 'hang' and variant in mode[1:]):
        with open(os.path.join(HERE, 'hanging'), 'a') as f:
            f.write(variant + '\n')
        while True:
            time.sleep(60)
    model = dict(vs=0, ps=4, cs=5)[rq['stage']]
    with open(os.path.join(out, variant + '.spv'), 'wb') as f:
        f.write(struct.pack('<5I', 0x07230203, 0x10000, 0, 2, 0) + struct.pack('<4I', 4 << 16 | 15, model, 1, 0x6d))
    with open(os.path.join(out, variant + '.vk'), 'w') as f:
        f.write('vk 1\n')
    vv = os.path.join(out, variant.split('-')[0] + '.vv')
    rows = [r for r in (open(vv).read().splitlines() if os.path.exists(vv) else []) if not r.startswith(variant + '\t')]
    with open(vv, 'w') as f:
        f.write('\n'.join(rows + [variant + '\t']) + '\n')
    print('{{"ok": true}}', flush=True)
'''


def alive(pid):
    try:
        return Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[0] != 'Z'
    except (FileNotFoundError, ProcessLookupError):
        return False


@unittest.skipUnless(zstd and shutil.which('cc') and shutil.which('git') and sys.platform.startswith('linux'),
                     'needs Linux, Python 3.14 (compression.zstd), cc and git')
class TranslatorWatchdog(unittest.TestCase):
    """A service that gives no reply within the request deadline is killed, its job is recorded as a failed 'timeout'
    translation and a new service continues; a translator that never answers fails the stage after a bounded number
    of restarts; cancelling during a hang ends at once. No translator process outlives seed_cli."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='totk-seed-watchdog-'))
        cls.romfs, cls.modules, cls.list, cls.keys = fixture(cls.tmp)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def service(self, name, mode):
        here = self.tmp / name
        (here / 'lib').mkdir(parents=True)
        (here / 'mode').write_text(mode)
        exe = here / 'lib/libtotk_xlate.so'
        exe.write_text(FAKE_SERVICE.format(python=sys.executable, here=str(here)))
        exe.chmod(0o755)
        return here

    def assert_no_service_left(self, here):
        pids = [int(p) for p in (here / 'pids').read_text().split()]
        self.assertTrue(pids)
        self.assertEqual([p for p in pids if alive(p)], [])

    def cmd(self, here, *extra):
        return [seed_cli(), '--romfs', self.romfs, '--rewritten', self.modules, '--out', here / 'out', '--libdir', here / 'lib',
                '--staging', here / 'staging', '--shader-list', self.list, '--any-count', *map(str, extra)]

    def test_hung_request_is_killed_recorded_and_skipped(self):
        hung = self.keys['vs'] + '-' + hashlib.sha256(b'{"attr":{},"fout":{},"img":{},"tex":{}}').hexdigest()[:12]
        here = self.service('one', f'hang {hung}')
        r = subprocess.run(self.cmd(here, '--workers', 2, '--request-timeout-ms', 1500), capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual((here / 'hanging').read_text().split(), [hung])
        log = (here / 'staging/shaders.log').read_text()
        self.assertRegex(log, rf'{hung}\tdefault\tvs\t"timeout: no reply in 1500 ms"')
        self.assertIn('failed 1\n', log)
        files = read_pack(here / 'out/seed.vkpack')
        self.assertNotIn(hung + '.spv', files)
        self.assertTrue(any(n.startswith(self.keys['vs']) and n.endswith('.spv') for n in files))   # its listed states
        self.assertEqual(sum(n.endswith('.spv') for n in files), sum(n.endswith('.vk') for n in files))
        self.assert_no_service_left(here)

    def test_translator_that_never_answers_fails_bounded(self):
        here = self.service('all', 'hang-all')
        # More jobs than the restart bound: 24 more listed states of one shader, so the bound (not the end of the
        # plan) ends the run. Every request hangs; every service is killed after 300 ms.
        lst = here / 'list.tsv'
        lst.write_text(self.list.read_text() + ''.join(f'V\t{self.keys["vs"]}\t{{"attr":{{"0":"float{i}"}}}}\n' for i in range(24)))
        cmd = self.cmd(here, '--workers', 2, '--request-timeout-ms', 300)
        cmd[cmd.index(self.list)] = lst
        began = time.monotonic()
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn('keeps exiting or hanging', r.stdout)
        self.assertLess(time.monotonic() - began, 30)
        # The two first services, then one per timeout below the bound of 16 restarts (the other worker may stop
        # before starting its last one).
        self.assertIn(len((here / 'pids').read_text().split()), (17, 18))
        self.assertFalse((here / 'out/seed.vkpack').exists())
        self.assert_no_service_left(here)

    def test_cancel_during_hang(self):
        here = self.service('cancel', 'hang-all')
        p = subprocess.Popen(self.cmd(here, '--workers', 3), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        try:
            for _ in range(600):
                if (here / 'hanging').is_file() and len((here / 'hanging').read_text().split()) == 3:
                    break
                time.sleep(0.05)
            else:
                self.fail('the services never reached their hang')
            began = time.monotonic()
            p.send_signal(signal.SIGTERM)
            out, _ = p.communicate(timeout=10)
        finally:
            if p.poll() is None:
                p.kill()
        self.assertEqual(p.returncode, 3, out)
        self.assertLess(time.monotonic() - began, 3)
        self.assertIn('"code":"cancelled"', out)
        self.assert_no_service_left(here)


@unittest.skipUnless((REF / 'extracted/v143/romfs/runtime-index.bin').is_file() and (REF / 'rewritten-android/v143').is_dir(),
                     f'reference work folder {REF} not present')
@unittest.skipUnless(zstd and shutil.which('cc') and shutil.which('git'), 'needs Python 3.14 (compression.zstd), cc and git')
class SeedOracle(unittest.TestCase):
    """Long: two full scans of the 17 GB RomFS (C and Python) plus the translation sample."""
    romfs = REF / 'extracted/v143/romfs'
    modules = REF / 'rewritten-android/v143'

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='totk-seed-oracle-'))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    @unittest.skipUnless(translator(), 'no host translator with --vulkan')
    def test_full_plan_matches_python(self):
        lst = GAME / 'assets/shader_list.tsv'
        dump = self.tmp / 'plan.tsv'
        r = run_cli(self.romfs, self.modules, self.tmp / 'plan', libdir(self.tmp), '--shader-list', lst, '--plan-out', dump, '--plan-only')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        assert_same_plan(self, dump, *python_plan(self.romfs, self.modules, lst))

    @unittest.skipUnless(translator(), 'no host translator with --vulkan')
    def test_shipped_shader_files_are_the_full_scan(self):
        """Without --full-scan the stage reads only the files of seed/shader_files.c (the pinned index's shader files).
        A full scan of the 17 GB image must regenerate that list byte for byte and give the very same inventory and plan."""
        lst = GAME / 'assets/shader_list.tsv'
        listed, full, files = self.tmp / 'plan-listed.tsv', self.tmp / 'plan-full.tsv', self.tmp / 'shader_files.c'
        r = run_cli(self.romfs, self.modules, self.tmp / 'listed', libdir(self.tmp), '--shader-list', lst, '--plan-out', listed, '--plan-only')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        r = run_cli(self.romfs, self.modules, self.tmp / 'full', libdir(self.tmp), '--shader-list', lst, '--plan-out', full, '--plan-only',
                    '--full-scan', '--files-out', files)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(files.read_text(), (SEED / 'shader_files.c').read_text())
        lines = lambda p, kind: [x for x in p.read_text().splitlines() if x.startswith(kind)]
        self.assertEqual(sorted(lines(listed, 'S\t')), sorted(lines(full, 'S\t')))   # table slot order varies with threads
        self.assertEqual(lines(listed, 'J\t'), lines(full, 'J\t'))

    @unittest.skipUnless(translator() and PC_SEED.is_dir(), f'no host translator with --vulkan or no PC seed at {PC_SEED}')
    def test_translated_sample_matches_pc_seed(self):
        """The PC seed was made with the shader list of its day, recorded in its manifest; its exact text comes from git."""
        manifest = REF / 'build/shader-seed-android/v143/seed-manifest.json'
        want = json.loads(manifest.read_text())['shader_list_sha256'] if manifest.is_file() else None
        lst = self.tmp / 'pc_list.tsv'
        for rev in subprocess.run(['git', '-C', ROOT, 'log', '--format=%H', '--', 'games/totk/assets/shader_list.tsv'],
                                  capture_output=True, text=True).stdout.split():
            text = subprocess.run(['git', '-C', ROOT, 'show', f'{rev}:games/totk/assets/shader_list.tsv'], capture_output=True).stdout
            if hashlib.sha256(text).hexdigest() == want:
                lst.write_bytes(text); break
        if not lst.is_file():
            self.skipTest('the PC seed\'s shader list is not in this checkout\'s history')
        out = self.tmp / 'seed'
        r = run_cli(self.romfs, self.modules, out, libdir(self.tmp), '--shader-list', lst, '--sample', SAMPLE)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        files = assert_pack_round_trip(self, out)
        report = compare_with_pc(files, PC_SEED)
        print(json.dumps(report, indent=1), file=sys.stderr)
        self.assertGreater(report['compared_variants'], 100)
        self.assertEqual(report['spv_differ'], report['spv_differ_storage_image_capability_only'])
        self.assertEqual(report['vk_differ'], 0)
        self.assertEqual(report['vv_rows_differ'], 0)


def _without_storage_write_capability(spv):
    """SPIR-V without its OpCapability StorageImageWriteWithoutFormat (56) instructions: the translator declares one per
    formatless storage image written since 2026-10-24 (e611b20a, tools/totk_xlate/patches/*formatless*); a seed
    translated before then lacks them and is otherwise the same."""
    out, i = bytearray(spv[:20]), 5
    while i * 4 < len(spv):
        word = struct.unpack_from('<I', spv, i * 4)[0]
        n = word >> 16
        if not n:
            return spv
        if not (word == (2 << 16) | 17 and struct.unpack_from('<I', spv, i * 4 + 4)[0] == 56):
            out += spv[i * 4:(i + n) * 4]
        i += n
    return bytes(out)


def compare_with_pc(files, pc):
    report = dict(compared_variants=0, missing_in_pc=0, spv_differ=0, spv_differ_storage_image_capability_only=0, vk_differ=0,
                  vv_rows_differ=0, examples=[])
    for name, payload in sorted(files.items()):
        if name.endswith('.vv'):
            theirs = pc / name
            if theirs.is_file():
                mine = dict(row.split('\t', 1) for row in payload.decode().splitlines())
                pcrows = dict(row.split('\t', 1) for row in theirs.read_text().splitlines())
                report['vv_rows_differ'] += sum(1 for v, deps in mine.items() if v in pcrows and pcrows[v] != deps)
            continue
        if not name.endswith('.spv'):
            continue
        variant = name[:-4]
        if not (pc / name).is_file():
            report['missing_in_pc'] += 1
            continue
        report['compared_variants'] += 1
        theirs = (pc / name).read_bytes()
        if theirs != payload:
            report['spv_differ'] += 1
            if _without_storage_write_capability(payload) == theirs:
                report['spv_differ_storage_image_capability_only'] += 1
            elif len(report['examples']) < 5:
                report['examples'].append(name)
        if (pc / (variant + '.vk')).read_bytes() != files[variant + '.vk']:
            report['vk_differ'] += 1
    return report


if __name__ == '__main__':
    unittest.main()
