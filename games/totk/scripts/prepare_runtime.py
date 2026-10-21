#!/usr/bin/env python3
"""Bind the native loader to the verified images: <game-data>/runtime-tables.bin (stays local, never committed).

The loader reads the file at start (runtime/loader.c load_tables), so the program itself holds nothing of the game.
The on-device setup writes the same bytes with its C port (android/setup/rewrite/tables.c)."""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'third_party/d2r'))
from relr import decode_relr
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--version', choices=('v0','v112','v121','v143'), default='v0')
# Linux / Android (android/build_android.py): modules rewritten with DM_HOST_TP=tpidr_el0 in their own folder, their own
# validation report, and the tables written next to the Android build.
parser.add_argument('--rewritten', help='folder of the rewritten modules (default rewritten/<version>)')
parser.add_argument('--report', help='validation report naming their hashes (default analysis/.../phase12-validation.json)')
parser.add_argument('--out', help='game-data folder for runtime-tables.bin (default build/game-data/<version>)')
parser.add_argument('--exefs', help='folder of the original modules (default extracted/<version>/exefs of the work folder '
                                    'the report\'s paths are relative to)')
args = parser.parse_args()
version = args.version
OUT = Path(args.out) if args.out else ROOT / f'build/game-data/{version}'
OUT.mkdir(parents=True, exist_ok=True)
REWRITTEN = Path(args.rewritten).resolve() if args.rewritten else ROOT / f'rewritten/{version}'
REPORT = Path(args.report) if args.report else ROOT / ('analysis/phase12-validation.json' if version == 'v0' else f'analysis/{version}-port/phase12-validation.json')
report = json.loads(REPORT.read_text())
assert report['status'] == 'pass' and report['rom_version'] == version
# The report names files relative to the work folder that made them: this checkout, or the folder holding the report.
BASES = [ROOT, *REPORT.resolve().parents]
def report_key(path):
    path = path.resolve()
    for base in BASES:
        for key in report['module_file_sha256']:
            if (base / key).resolve() == path:
                return base, key
    raise AssertionError(f'{path} is not named by {REPORT}')
if version == 'v112':
    expected = 'f04fbebdaacbcde0bcca20cbe0caff06bd51f7c8f904d217e5f17efe3308d1ac'
    assert report['module_file_sha256']['extracted/v112/exefs/main'] == expected, 'Unsupported 1.1.2 main image'
if version == 'v121':
    assert report['module_file_sha256']['extracted/v121/exefs/main'] == 'e5aa8d48e68cbb82ed4416c7b6b29a46fe4a998ee149d6dbf979164c628efcb6', 'Unsupported 1.2.1 main image'
if version == 'v143':
    assert report['module_file_sha256']['extracted/v143/exefs/main'] == 'e84a90d551fed0824b4b74933b9c6d043c458521363a7cea58b2faca1822dc9e', 'Unsupported 1.4.3 main image'
modules = []
for name in ['main', 'subsdk0', 'sdk']:
    path = REWRITTEN / f'{name}.patched.bin'
    image = path.read_bytes()
    digest = hashlib.sha256(image).hexdigest()
    work, key = report_key(path)
    assert digest == report['module_file_sha256'][key], 'Unverified rewrite'
    EXEFS = Path(args.exefs) if args.exefs else work / f'extracted/{version}/exefs'
    layout = {}
    for line in (REWRITTEN / f'{name}.fixups.tsv').read_text().splitlines():
        _, key, value = line.split()
        layout[key] = int(value, 0)
    mod0 = struct.unpack_from('<I', image, 4)[0]
    assert image[mod0:mod0 + 4] == b'MOD0'
    dyn = mod0 + struct.unpack_from('<i', image, mod0 + 4)[0]
    tags = {}
    for offset in range(dyn, dyn + 4096 * 16, 16):
        tag, value = struct.unpack_from('<qQ', image, offset)
        if tag == 0: break
        tags[tag] = value
    else: raise ValueError('Unterminated dynamic table')
    if 4 in tags:
        nsym = struct.unpack_from('<I', image, tags[4] + 4)[0]
    else:
        # Newer SDK modules carry GNU hash only. NSO's verified dynsym extent
        # supplies the exact count, as in prepare_modules.py's inventory gate.
        raw_header = (EXEFS / name).read_bytes()[:256]
        _, _, symbol_offset, symbol_size = struct.unpack_from('<IIII', raw_header, 0x90)
        assert layout['RO_OFF'] + symbol_offset == tags[6] and symbol_size % 24 == 0
        assert 0 < symbol_size <= layout['DATA_OFF'] - tags[6]
        nsym = symbol_size // 24
    assert tags[11] == 24 and tags[9] == 24 and tags[20] == 7
    symbols = []
    for i in range(nsym):
        no, info, other, section, value, size = struct.unpack_from('<IBBHQQ', image, tags[6] + 24 * i)
        end = image.index(0, tags[5] + no, tags[5] + tags[10])
        symbols.append(dict(name=image[tags[5] + no:end].decode(), value=value, size=size,
                            defined=bool(section), binding=info >> 4, type=info & 15))
    relocs = [dict(where=where, kind=1027, symbol=0, addend=addend)
              for where, addend in decode_relr(image, tags, layout['DATA_OFF'], layout['KEY_PAGE_OFF'])]
    relocated = {reloc['where'] for reloc in relocs}
    for address, size in [(tags[7], tags[8]), (tags[23], tags[2])]:
        assert size % 24 == 0
        for offset in range(address, address + size, 24):
            where, info, addend = struct.unpack_from('<QQq', image, offset)
            kind, si = info & 0xffffffff, info >> 32
            assert kind in [257, 1025, 1026, 1027] and si < nsym
            assert layout['DATA_OFF'] <= where <= layout['KEY_PAGE_OFF'] - 8
            assert kind != 1027 or 0 <= addend < layout['KEY_PAGE_OFF']
            assert where not in relocated, 'Duplicate RELR/RELA relocation'
            relocated.add(where)
            relocs.append(dict(where=where, kind=kind, symbol=si, addend=addend))
    gates = []
    for line in (REWRITTEN / f'{name}.gates.tsv').read_text().splitlines():
        ret, kind, arg, original = line.split()
        assert kind in ['cntpct', 'cntvct', 'svc']
        gates.append(dict(ret=int(ret, 0), timer=kind != 'svc', arg=int(arg), original=int(original, 0)))
    gates.sort(key=lambda g: g['ret'])   # runtime_dispatch uses binary search
    assert len({g['ret'] for g in gates}) == len(gates), 'duplicate gate return address'
    # Match each compiler's actual DT_INIT state load before decoding it.
    flag_reg, load_offset = (19, 20) if version == 'v143' else (20, 24)
    a, w = struct.unpack_from('<I', image, tags[12]+16)[0], struct.unpack_from('<I', image, tags[12]+load_offset)[0]
    assert a & 0x9f00001f == 0x90000000 | flag_reg and w & 0xffc003ff == 0xb9400008 | (flag_reg << 5)
    imm = ((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2)
    imm = (imm ^ (1 << 20)) - (1 << 20)
    ctor_flag = ((tags[12]+16) & ~4095) + (imm << 12) + ((w >> 10) & 0xfff)*4
    assert layout['DATA_OFF'] <= ctor_flag < layout['KEY_PAGE_OFF']
    modules.append(dict(name=name, sha256=digest, size=len(image), ctor_flag=ctor_flag, layout=layout, tags=tags,
                        symbols=symbols, relocs=relocs, gates=gates))

# Find main's ORIGINAL PLT stub addressing the relocated GetSystemTick slot.
main = modules[0]
target = '_ZN2nn2os13GetSystemTickEv'
slot = next(r['where'] for r in main['relocs'] if main['symbols'][r['symbol']]['name'] == target and r['kind'] == 1026)
image = (REWRITTEN / 'main.patched.bin').read_bytes()
plt = []
for address in range(main['layout']['TEXT_END'] - 0x10000, main['layout']['TEXT_END'] - 12, 4):
    a, b, c, d = struct.unpack_from('<IIII', image, address)
    if a & 0x9f00001f != 0x90000010 or b & 0xffc003ff != 0xf9400211 or d != 0xd61f0220: continue
    imm = ((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2)
    imm = (imm ^ (1 << 20)) - (1 << 20)
    referenced = (address & ~4095) + (imm << 12) + ((b >> 10) & 0xfff) * 8
    if referenced == slot: plt.append(address)
assert len(plt) == 1, f'PLT matches {plt}'
main['probe_plt'] = plt[0]
main['probe_slot'] = slot

# runtime-tables.bin in the layouts of runtime/loader.c load_tables: a 64-byte header, one 208-byte record per module, each
# module's symbols, relocations and gates (24 bytes each, 8-byte aligned), then the symbol names.
HEADER, RECORD = 64, 208
strings, arrays, records = bytearray(), bytearray(), []
def place(blob):
    arrays.extend(bytes(-len(arrays) % 8))
    arrays.extend(blob)
    return HEADER + RECORD * len(modules) + len(arrays) - len(blob)
for m in modules:
    names = []
    for s in m['symbols']:
        names.append(len(strings))
        strings.extend(s['name'].encode() + b'\0')
    symbols = b''.join(struct.pack('<QQIBBBx', s['value'], s['size'], n, int(s['defined']), s['binding'], s['type'])
                       for s, n in zip(m['symbols'], names))
    relocs = b''.join(struct.pack('<QIIq', r['where'], r['kind'], r['symbol'], r['addend']) for r in m['relocs'])
    gates = b''.join(struct.pack('<QQII', g['ret'], g['original'], int(g['timer']), g['arg']) for g in m['gates'])
    l, t = m['layout'], m['tags']
    records.append(struct.pack('<16s72s13Q4I', m['name'].encode(), m['sha256'].encode(), m['size'], l['TEXT_END'], l['RO_OFF'],
                               l['DATA_OFF'], l['PATCH_OFF'] % 2**64, l['KEY_PAGE_OFF'], t.get(12, 0), m['ctor_flag'], t.get(25, 0),
                               t.get(27, 0), place(symbols), place(relocs), place(gates), len(m['symbols']), len(m['relocs']),
                               len(m['gates']), 0))
arrays.extend(bytes(-len(arrays) % 8))
strings_offset = HEADER + RECORD * len(modules) + len(arrays)
size = strings_offset + len(strings)
header = struct.pack('<8s8sIIQQQQQ', b'TOTKTAB1', version.encode(), len(modules), 1, main['probe_plt'], slot, strings_offset,
                     len(strings), size)
assert len(header) == HEADER and all(len(r) == RECORD for r in records)
tables = OUT / 'runtime-tables.bin'
tables.with_suffix('.partial').write_bytes(header + b''.join(records) + arrays + strings)
tables.with_suffix('.partial').replace(tables)
# Details are evidence, not additional source code.
(OUT / 'runtime-layout.json' if args.out else ROOT / ('analysis/runtime-layout.json' if version == 'v0' else f'analysis/{version}-port/runtime-layout.json')).write_text(json.dumps(modules, indent=2) + '\n')
print(json.dumps(dict(modules=len(modules), relocations=sum(len(m['relocs']) for m in modules), original_main_plt=hex(plt[0]), original_main_slot=hex(slot))))
