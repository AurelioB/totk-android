"""The runtime's game data folder without game files: runtime/game_data.c (KEY VALUE files) and loader.c load_tables
(runtime-tables.bin, TOTKTAB1) compiled for the host, on synthetic files, including the pinned-build refusals.

TOTK_REFERENCE_GAME_DATA=<game-data/v143 folder> additionally parses real 1.4.3 tables in the pinned configuration
(read only; never committed)."""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / 'games/totk/runtime'
PINS_SCRIPT = ROOT / 'games/totk/scripts/make_pins_header.py'
PINS_JSON = ROOT / 'games/totk/assets/expected-v1.4.3.json'

READER = r'''
#include "game_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "FAIL: %s\n", message); exit(3); }
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    if (!strcmp(argv[3], "number")) printf("%llu\n", (unsigned long long)runtime_game_number(argv[1], argv[2]));
    else if (!strcmp(argv[3], "sha256")) printf("%s\n", runtime_game_sha256(argv[1], argv[2]));
    else printf("%s\n", runtime_game_text(argv[1], argv[2]));
    return 0;
}
'''

TABLES_PRELUDE = r'''
#include "runtime.h"
#include "game_data.h"
#include "game_version.h"
#include "host_os.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if TOTK_PINNED
#include "totk_pins.h"
#endif
_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "FAIL: %s\n", message); exit(3); }
static void need(bool c, const char *m) { if (!c) runtime_fail(m); }
'''
TABLES_MAIN = r'''
int main(void) {
    load_tables();
    printf("probe %llu %llu\n", (unsigned long long)probe_plt, (unsigned long long)probe_slot);
    for (unsigned i = 0; i < NM; i++) {
        const Module *m = &modules[i];
        printf("module %s %s %zu %llu %u %u %u", m->name, m->digest, m->file_size, (unsigned long long)m->key_off, m->nsym, m->nreloc, m->ngate);
        for (unsigned k = 0; k < m->ngate; k++) printf(" g%llx", (unsigned long long)m->gates[k].ret);
        if (m->nsym) printf(" s=%s", m->symbols[m->nsym - 1].name);
        if (m->nreloc) printf(" r=%llx", (unsigned long long)m->relocs[0].where);
        printf("\n");
    }
    return 0;
}
'''


def tables(version=b'v143', digests=None, gates=((0x30, 1, 0, 7), (0x10, 2, 1, 8)), magic=b'TOTKTAB1'):
    """A synthetic runtime-tables.bin in scripts/prepare_runtime.py's layout (3 modules: main, subsdk0, sdk)."""
    names = ('main', 'subsdk0', 'sdk')
    digests = digests or [hashlib.sha256(n.encode()).hexdigest() for n in names]
    strings, arrays, records = bytearray(), bytearray(), []
    def place(blob):
        arrays.extend(bytes(-len(arrays) % 8)); arrays.extend(blob)
        return 64 + 208 * 3 + len(arrays) - len(blob)
    for i, name in enumerate(names):
        syms = []
        for s in ('', f'sym_{name}'):
            syms.append(struct.pack('<QQIBBBx', 0x100 * i, 8, len(strings), 1, 1, 2)); strings.extend(s.encode() + b'\0')
        relocs = struct.pack('<QIIq', 0x4000 + i, 1027, 0, 0x20)
        gate_blob = b''.join(struct.pack('<QQII', *g) for g in gates)
        records.append(struct.pack('<16s72s13Q4I', name.encode(), digests[i].encode(), 0x10000 + i, 0x1000, 0x2000, 0x3000, 0x9000, 0x8000,
                                   0x40, 0x3010, 0x3000, 8, place(b''.join(syms)), place(relocs), place(gate_blob), 2, 1, len(gates), 0))
    arrays.extend(bytes(-len(arrays) % 8))
    so = 64 + 208 * 3 + len(arrays)
    head = struct.pack('<8s8sIIQQQQQ', magic, version, 3, 1, 0x1234, 0x5678, so, len(strings), so + len(strings))
    return head + b''.join(records) + bytes(arrays) + bytes(strings)


class GameData(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cc = shlex.split(os.environ.get('CC', 'cc'))
        if not shutil.which(cls.cc[0]):
            raise unittest.SkipTest('a native C compiler is required')
        scratch = Path(os.environ.get('TMPDIR', ROOT / '.scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='android-game-data-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.dir = Path(cls.temp.name)
        (cls.dir / 'reader.c').write_text(READER)
        cls.reader = cls.dir / 'reader'
        cls.build([cls.dir / 'reader.c', RUNTIME / 'game_data.c'], cls.reader)
        loader = (RUNTIME / 'loader.c').read_text()
        block = loader[loader.index("/* The three modules' tables"):loader.index('static size_t page_up(')]
        (cls.dir / 'tables.c').write_text(TABLES_PRELUDE + block + TABLES_MAIN)
        # host_os.c's portable SHA-256 only (the rest of that file is arm64 Linux / Android specific)
        host = (RUNTIME / 'host_os.c').read_text()
        (cls.dir / 'sha.c').write_text('#include "host_os.h"\n#include <string.h>\n' + host[host.index('/* SHA-256 (FIPS 180-4)'):host.rindex('#endif')])
        cls.tables = cls.dir / 'tables'
        cls.build(cls.table_sources(), cls.tables)

    @classmethod
    def table_sources(cls):
        return [cls.dir / 'tables.c', RUNTIME / 'game_data.c', cls.dir / 'sha.c']

    @classmethod
    def build(cls, sources, out, defines=(), include=None):
        cmd = [*cls.cc, '-std=gnu11', '-O1', '-Wall', '-Werror', '-Wno-unused-function', '-D_GNU_SOURCE', '-DTOTK_GAME_V143=1', *defines,
               '-I', str(RUNTIME), *(['-I', str(include)] if include else []), *map(str, sources), '-o', str(out), '-lpthread']
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(' '.join(cmd) + '\n' + r.stdout + r.stderr)

    def run_in(self, exe, folder, *args, env=None):
        environment = {k: v for k, v in os.environ.items() if not k.startswith('TOTK_')}
        environment.update(env if env is not None else {'TOTK_GAME_DATA': str(folder)})
        return subprocess.run([str(exe), *args], capture_output=True, text=True, env=environment, cwd=folder)

    def folder(self, files=None):
        d = Path(tempfile.mkdtemp(dir=self.dir))
        for name, data in (files or {}).items():
            (d / name).write_bytes(data if isinstance(data, bytes) else data.encode())
        return d

    # ---- game_data.c ----
    def test_values(self):
        sha = 'ab' * 32
        d = self.folder({'romfs-config.txt': f'# comment\nROMFS_IMAGE_SIZE 17052625232\r\nROMFS_IMAGE_SHA256 {sha}\nNAME some text\n'})
        name = 'romfs-config.txt'
        self.assertEqual(self.run_in(self.reader, d, name, 'ROMFS_IMAGE_SIZE', 'number').stdout, '17052625232\n')
        self.assertEqual(self.run_in(self.reader, d, name, 'ROMFS_IMAGE_SHA256', 'sha256').stdout, sha + '\n')
        self.assertEqual(self.run_in(self.reader, d, name, 'NAME', 'text').stdout, 'some text\n')

    def test_refusals(self):
        d = self.folder({'a.txt': 'BAD -1\nSHA ABCD\nLONELY\n'})
        for key, kind in (('BAD', 'number'), ('SHA', 'sha256'), ('MISSING', 'text')):
            with self.subTest(key=key):
                self.assertEqual(self.run_in(self.reader, d, 'a.txt', key, kind).returncode, 3)
        self.assertEqual(self.run_in(self.reader, d, 'absent.txt', 'X', 'text').returncode, 3)

    def test_default_folder_is_under_the_current_one(self):
        d = self.folder()
        (d / 'build/game-data/v143').mkdir(parents=True)
        (d / 'build/game-data/v143/x.txt').write_text('K 5\n')
        self.assertEqual(self.run_in(self.reader, d, 'x.txt', 'K', 'number', env={}).stdout, '5\n')   # host default

    # ---- loader.c load_tables ----
    def test_tables_decode_and_sort_gates(self):
        r = self.run_in(self.tables, self.folder({'runtime-tables.bin': tables()}))
        self.assertEqual(r.returncode, 0, r.stderr)
        lines = r.stdout.splitlines()
        self.assertEqual(lines[0], f'probe {0x1234} {0x5678}')
        for i, name in enumerate(('main', 'subsdk0', 'sdk')):
            self.assertEqual(lines[1 + i].split()[1:8], [name, hashlib.sha256(name.encode()).hexdigest(), str(0x10000 + i), str(0x8000), '2', '1', '2'])
            self.assertIn(' g10 g30 ', lines[1 + i] + ' ')   # sorted for runtime_dispatch's binary search
            self.assertIn(f's=sym_{name}', lines[1 + i])
            self.assertIn(f'r={0x4000 + i:x}', lines[1 + i])

    def test_tables_refused(self):
        good = tables()
        cases = {'magic': tables(magic=b'TOTKTAB2'), 'version': tables(version=b'v121'), 'truncated': good[:-1],
                 'size field': good + b'\0', 'duplicate gates': tables(gates=((0x10, 1, 0, 7), (0x10, 2, 1, 8))),
                 'string pool not terminated': good[:-1] + b'x'}
        for what, data in cases.items():
            with self.subTest(what=what):
                r = self.run_in(self.tables, self.folder({'runtime-tables.bin': data}))
                self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
        self.assertEqual(self.run_in(self.tables, self.folder()).returncode, 3)   # missing

    def pinned_build(self, digests, tables_sha=None):
        include = Path(tempfile.mkdtemp(dir=self.dir))
        rows = {'MAIN': digests[0], 'SUBSDK0': digests[1], 'SDK': digests[2]}
        text = ''.join(f'#define TOTK_PIN_ANDROID_REWRITTEN_SHA256_{k} "{v}"\n' for k, v in rows.items())
        if tables_sha:
            text += f'#define TOTK_PIN_ANDROID_RUNTIME_TABLES_SHA256 "{tables_sha}"\n'
        (include / 'totk_pins.h').write_text(text)
        exe = include / 'tables'
        self.build(self.table_sources(), exe, ('-DTOTK_PINNED=1',), include)
        return exe

    def test_pinned_build_refuses_other_tables_and_modules(self):
        data = tables()
        digests = [hashlib.sha256(n.encode()).hexdigest() for n in ('main', 'subsdk0', 'sdk')]
        good = self.pinned_build(digests, hashlib.sha256(data).hexdigest())
        self.assertEqual(self.run_in(good, self.folder({'runtime-tables.bin': data})).returncode, 0)
        wrong_file = self.pinned_build(digests, 'cd' * 32)
        self.assertEqual(self.run_in(wrong_file, self.folder({'runtime-tables.bin': data})).returncode, 3)
        wrong_module = self.pinned_build(digests[:2] + ['ef' * 32])
        r = self.run_in(wrong_module, self.folder({'runtime-tables.bin': data}))
        self.assertEqual(r.returncode, 3)
        self.assertIn('pinned', r.stderr)

    @unittest.skipUnless(os.environ.get('TOTK_REFERENCE_GAME_DATA'), 'set TOTK_REFERENCE_GAME_DATA to a real game-data/v143 folder')
    def test_reference_tables_in_pinned_configuration(self):
        folder = Path(os.environ['TOTK_REFERENCE_GAME_DATA'])
        include = Path(tempfile.mkdtemp(dir=self.dir))
        subprocess.run([sys.executable, PINS_SCRIPT, PINS_JSON, include / 'totk_pins.h'], check=True)
        exe = include / 'tables'
        self.build(self.table_sources(), exe, ('-DTOTK_PINNED=1',), include)
        r = self.run_in(exe, folder)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(len(r.stdout.splitlines()), 4)


if __name__ == '__main__':
    unittest.main()
