"""Shader preparation uses local game containers; seed import preserves user variants."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT/'games/totk/scripts/prepare_shader_seed.py'
spec = importlib.util.spec_from_file_location('prepare_shader_seed', SCRIPT)
seed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(seed)


def bnsh():
    b = bytearray(0x3000)
    b[:4] = b'BNSH'; b[12:14] = b'\xff\xfe'; b[96:100] = b'grsc'
    struct.pack_into('<I', b, 28, len(b))
    struct.pack_into('<IQ', b, 124, 1, 0xc0)
    struct.pack_into('<Q', b, 0xc0+16, 0x100)
    for i, (stage, rel) in enumerate([(0, 8), (1, 40), (5, 48)]):
        header, control, code = 0x200+i*64, 0x400+i*0x880, 0x2000+i*0x100
        struct.pack_into('<Q', b, 0x100+rel, header)
        struct.pack_into('<QQII', b, header+8, control, code, 0x100, 0x880)
        struct.pack_into('<II', b, control+0x700, 0x80, 0x100)
        struct.pack_into('<I', b, control+0x714, stage)
        struct.pack_into('<III', b, control+0x748, 8, 8, 1)
        b[code:code+0x100] = bytes([i+1])*0x100
    return b


def bnsh_botw():
    """Breath of the Wild's layout: control sections of 0x7d1 bytes that say so at +0x7c8, each followed by other data."""
    b = bnsh()
    for i in range(3):
        control = 0x400+i*0x880
        struct.pack_into('<I', b, control+0x20, 0x7d1)
        b[control+0x7d1:control+0x880] = bytes([0xee])*(0x880-0x7d1)   # what follows: not the shader's
        struct.pack_into('<I', b, 0x200+i*64+8+20, 0x7d1)   # the program table's control size
    return b


class ShaderInventory(unittest.TestCase):
    def test_the_key_hashes_the_control_sections_own_size_as_the_runtime_does(self):
        self.assertEqual(seed.control_key_len(bytes(0x880)), 0x880)              # no size word: the NVN 55 section
        self.assertEqual(seed.control_key_len(bytes(0x20)+struct.pack('<I', 0x88c)+bytes(0x860)), 0x880)   # 0x88c: hashed as 0x880
        self.assertEqual(seed.control_key_len(bytes(0x20)+struct.pack('<I', 0x7d1)+bytes(0x860)), 0x7d1)
        self.assertEqual(seed.control_key_len(bytes(0x20)+struct.pack('<I', 0x848)+bytes(0x860)), 0x848)
        agl = bytes(0x14)+b'\x40\0\0\0'+bytes(8)+struct.pack('<I', 0x848)+bytes(0x848-0x24)
        self.assertEqual(seed.bound_control(agl), agl)                    # Tears of the Kingdom: as in the archive
        rows = list(seed.bnsh_shaders(bnsh_botw()))
        self.assertEqual([(r[1], len(r[2])) for r in rows], [('vs', 0x7d1), ('ps', 0x7d1), ('cs', 0x7d1)])
        for key, _, control, code in rows:
            self.assertEqual(key, hashlib.sha256(control+code).hexdigest())
            self.assertNotIn(0xee, control)

    def test_sarc_members_named_with_a_leading_slash_are_accepted(self):
        name = b'/x.bgdata\0\0\0\0\0\0\0'
        b = bytearray(b'SARC' + struct.pack('<HHIIHH', 0x14, 0xfeff, 0, 0x100, 0x100, 0))   # header, BOM, size, data offset, version
        b += b'SFAT' + struct.pack('<HHI', 0xc, 1, 0x65) + struct.pack('<4I', 0, 0x01000000, 0, 4)
        b += b'SFNT' + struct.pack('<HH', 8, 0) + name
        b += bytes(0x100-len(b)) + b'DATA'
        struct.pack_into('<I', b, 8, len(b))
        self.assertEqual(seed.sarc(bytes(b)), {'x.bgdata': b'DATA'})

    def test_actual_stage_offsets_and_runtime_hash_contract(self):
        rows = list(seed.bnsh_shaders(bnsh()))
        self.assertEqual([r[1] for r in rows], ['vs', 'ps', 'cs'])
        for key, _, control, code in rows:
            self.assertEqual(key, hashlib.sha256(control+code).hexdigest())

    def test_agl_shader_packages_give_control_and_code_through_their_records(self):
        # BAHS header: start of the records 0x100, code pool at 0x2000 (size 0x400). A record is <u64 code offset in the
        # pool, u32 control size, i32 offset of the control from the record>; a later record may reuse an earlier control.
        b = bytearray(0x2400)
        b[:4] = b'BAHS'; struct.pack_into('<8I', b, 0, 0x53484142, 9, len(b), 5, 0, 0x100, 0x400, 0x2000)
        def control(stage, length):
            c = bytearray(0x880); struct.pack_into('<II', c, 0x700, 0x40, length); struct.pack_into('<I', c, 0x714, stage)
            return bytes(c)
        struct.pack_into('<QIi', b, 0x100, 0x000, 0x880, 0x60); b[0x160:0x160+0x880] = control(0, 0x100)
        struct.pack_into('<QIi', b, 0x1000, 0x100, 0x88c, 0x60); b[0x1060:0x1060+0x880] = control(1, 0x200)  # 0x88c: hashed as 0x880
        struct.pack_into('<QIi', b, 0x1a00, 0x300, 0x880, 0x160-0x1a00)   # reuses the first control
        for at, fill in ((0x2000, 1), (0x2100, 2), (0x2300, 3)):
            b[at:at+0x100] = bytes([fill])*0x100
        got = {k: (stage, len(code)) for k, stage, _, code in seed.sharcb_shaders(bytes(b))}
        want = {hashlib.sha256(control(0, 0x100)+bytes([1])*0x100).hexdigest(): ('vs', 0x100),
                hashlib.sha256(control(1, 0x200)+bytes([2])*0x100+bytes(0x100)).hexdigest(): ('ps', 0x200),
                hashlib.sha256(control(0, 0x100)+bytes([3])*0x100).hexdigest(): ('vs', 0x100)}
        self.assertEqual(got, want)

    def test_agl_rejects_a_record_overlapping_its_own_control(self):
        # Regression for agl_technique_pfx's bogus ps identity 52e08b02...:
        # size bytes inside package metadata looked like a record with rel=0.
        for relative in (0, 4, 12, -4):
            with self.subTest(relative=relative):
                b = bytearray(0x1400)
                struct.pack_into('<8I', b, 0, 0x53484142, 9, len(b), 5, 0, 0x100, 0x400, 0x1000)
                control_at = 0x200 + relative
                struct.pack_into('<II', b, control_at + 0x700, 1, 0x100)
                struct.pack_into('<I', b, control_at + 0x714, 1)
                struct.pack_into('<QIi', b, 0x200, 0, 0x880, relative)
                self.assertEqual(list(seed.sharcb_shaders(bytes(b))), [])

    def test_agl_accepts_adjacent_records_and_touching_control_boundaries(self):
        b = bytearray(0x1300)
        struct.pack_into('<8I', b, 0, 0x53484142, 9, len(b), 5, 0, 0x100, 0x300, 0x1000)
        # Two adjacent records; the control begins immediately after the second.
        # A third record starts exactly at the shared control's exclusive end.
        for record, code_offset in ((0x100, 0), (0x110, 0x100), (0x9a0, 0x200)):
            struct.pack_into('<QIi', b, record, code_offset, 0x880, 0x120 - record)
        struct.pack_into('<II', b, 0x120 + 0x700, 0x40, 0x100)
        struct.pack_into('<I', b, 0x120 + 0x714, 0)
        for i in range(3):
            b[0x1000 + 0x100 * i:0x1100 + 0x100 * i] = bytes([i + 1]) * 0x100
        got = list(seed.sharcb_shaders(bytes(b)))
        self.assertEqual(len(got), 3)
        self.assertEqual([stage for _, stage, _, _ in got], ['vs'] * 3)
        self.assertEqual({code for _, _, _, code in got}, {bytes([i + 1]) * 0x100 for i in range(3)})

    def test_prepared_spirv_stage_must_match_requested_stage(self):
        def module(model):
            return struct.pack('<9I', 0x07230203, 0x00010300, 0, 2, 0, (4 << 16) | 15, model, 1, 0)
        for stage, model in (('vs', 0), ('ps', 4), ('cs', 5)):
            seed.validate_spirv_stage(module(model), stage)
        with self.assertRaisesRegex(ValueError, 'stage mismatch'):
            seed.validate_spirv_stage(module(0), 'ps')
        with self.assertRaisesRegex(ValueError, 'stage mismatch'):
            seed.validate_spirv_stage(module(4), 'vs')

    def test_listed_bind_states_become_jobs_for_shaders_in_this_game(self):
        found = {'a'*64: ('vs', b'', b'', 'Shader/x'), 'b'*64: ('ps', b'', b'', 'Effect/y')}
        listed = {('a'*64, '{"attr":{"0":"sint"},"fout":{},"img":{},"tex":{}}'),
                  ('b'*64, '{"attr":{},"fout":{"0":"uint"},"img":{},"tex":{"1":"cube"}}'),
                  ('c'*64, '{"attr":{},"fout":{},"img":{},"tex":{}}')}   # a shader this game does not have
        jobs = seed.plan(found, startup={'a'*64}, listed=listed)
        # every startup shader gets the default bind state first; listed states follow, only for shaders found here
        self.assertEqual([(k[0], json.dumps(a, sort_keys=True), why) for k, a, _, why in jobs[:1]],
                         [('a', json.dumps(seed.ANSWERS, sort_keys=True), 'default')])
        self.assertEqual(sorted((k[0], why) for k, _, _, why in jobs), [('a', 'default'), ('a', 'listed'), ('b', 'listed')])
        for key, answers, variant, _ in jobs:
            self.assertEqual(set(answers), {'tex', 'img', 'attr', 'fout'})
            self.assertEqual(variant, key+'-'+seed.suffix(answers))
        self.assertEqual(seed.suffix(seed.ANSWERS), seed.SUFFIX)

    def test_located_shaders_are_read_from_the_named_place_and_checked_against_their_key(self):
        control = bytearray(0x880)
        struct.pack_into('<II', control, 0x700, 0x40, 0x100); struct.pack_into('<I', control, 0x714, 1)
        code = bytes(range(256))
        blob = bytes(0x200) + bytes(control) + bytes(0x80) + code
        key = hashlib.sha256(bytes(control)+code).hexdigest()
        sources = {'Lib/x.sarc.zs/y.sharcb': blob}
        good = seed.located({(key, 'Lib/x.sarc.zs/y.sharcb', 0x200, 0xb00, 0x100)}, sources.get)
        self.assertEqual(list(good), [key]); self.assertEqual(good[key][0], 'ps')
        wrong = seed.located({('f'*64, 'Lib/x.sarc.zs/y.sharcb', 0x200, 0xb00, 0x100)}, sources.get)
        self.assertEqual(wrong, {})   # bytes of another game version: not used
        self.assertEqual(seed.located({(key, 'Lib/missing', 0x200, 0xb00, 0x100)}, sources.get), {})

    def test_truncated_and_out_of_range_container_is_rejected(self):
        for data in [bnsh()[:-1], bnsh()]:
            if len(data) == 0x3000:
                struct.pack_into('<Q', data, 0xc0+16, 0x2fff)
            with self.assertRaises(ValueError):
                list(seed.bnsh_shaders(data))


class ShaderPreparation(unittest.TestCase):
    def test_stage_mismatch_aborts_preparation_instead_of_skipping_a_shader(self):
        key, stage, control, code = next(seed.bnsh_shaders(bnsh()))

        class Service:
            def __init__(self, *args, **kwargs): self.stdin = self.stdout = self
            def write(self, line): pass
            def flush(self): pass
            def close(self): pass
            def wait(self): return 0
            def readline(self):
                return '{"ok":false,"stage_mismatch":true,"error":"wrong executable stage"}\n'

        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root/'index.json').write_text(json.dumps(dict(romfs_sha256='c'*64)))
            binary = root/'translator'; binary.write_bytes(b'translator')
            output = root/'prepared'
            with mock.patch.dict(seed.GAMES['totk'], min_shaders=1), \
                    mock.patch.object(seed, 'inventory', return_value=(
                        {}, {key: (stage, control, code, 'Shader/test')}, {key}, [])), \
                    mock.patch.object(seed, 'source_reader', return_value=lambda _: None), \
                    mock.patch.object(seed.os, 'cpu_count', return_value=1), \
                    mock.patch.object(seed.subprocess, 'Popen', side_effect=Service):
                with self.assertRaisesRegex(ValueError, 'stage mismatch'):
                    seed.prepare(root, output, binary, binary, 'v143', root/'missing-list', keep_spirv=True)
            self.assertFalse(output.exists())

    def test_early_fragment_tests_uses_only_the_fragment_control_byte(self):
        shaders, expected, requests = {}, {}, []
        # NVN control: stage u32 at 0x714, early tests u8 at 0x718, post-depth
        # coverage u8 at 0x719, writes-depth u8 at 0x71c. The latter two do not
        # imply early tests; non-fragment and shorter-layout controls never enable the job flag.
        for early, coverage, depth in [(0, 0, 0), (0, 1, 1), (1, 0, 0), (0x80, 0, 0)]:
            data = bnsh()
            for i in range(3):
                control = 0x400+i*0x880
                data[control+0x718] = early
                data[control+0x719] = coverage
                data[control+0x71c] = depth
            for key, stage, control, code in seed.bnsh_shaders(data):
                shaders[key] = (stage, control, code, 'Shader/test')
                expected[key] = stage == 'ps' and early != 0
                self.assertEqual(key, hashlib.sha256(control+code).hexdigest())
        self.assertEqual(len(shaders), 12)   # the original control already distinguishes every flag combination
        for control_len in (0x7d1, 0x848):   # BOTW layouts do not define the NVN 0x880 early-tests byte
            data = bnsh()
            for i in range(3):
                control = 0x400+i*0x880
                struct.pack_into('<I', data, control+0x20, control_len)
                struct.pack_into('<I', data, 0x200+i*64+8+20, control_len)
                data[control+0x718] = 1
            for key, stage, control, code in seed.bnsh_shaders(data):
                self.assertEqual(len(control), control_len)
                shaders[key] = (stage, control, code, 'Shader/short-control')
                expected[key] = False
                self.assertEqual(key, hashlib.sha256(control+code).hexdigest())
        self.assertEqual(len(shaders), 18)

        class Service:
            def __init__(self, *args, **kwargs):
                self.stdin = self.stdout = self

            def write(self, line):
                request = json.loads(line)
                requests.append(request)
                base = Path(request['code']).with_suffix('')
                base.with_name(base.name+'_b.metal').write_text('// synthetic shader')
                base.with_name(base.name+'_b.json').write_text(json.dumps(
                    dict(chain=seed.CHAIN, stage=request['stage'], deps=seed.ANSWERS)))

            def flush(self): pass
            def close(self): pass
            def wait(self): return 0
            def readline(self): return '{"ok":true}\n'

        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); romfs = root/'romfs'; romfs.mkdir()
            (romfs/'index.json').write_text(json.dumps(dict(romfs_sha256='c'*64)))
            translator, cross, shared = [root/name for name in ['translator', 'spirv-cross', 'list.tsv']]
            for path in [translator, cross, shared]: path.write_text('')
            output = root/'seed'
            with mock.patch.dict(seed.GAMES['totk'], min_shaders=1), \
                    mock.patch.object(seed, 'inventory', return_value=({}, shaders, set(shaders), [])), \
                    mock.patch.object(seed, 'source_reader', return_value=lambda _: None), \
                    mock.patch.object(seed.os, 'cpu_count', return_value=1), \
                    mock.patch.object(seed.subprocess, 'Popen', side_effect=Service):
                manifest = seed.prepare(romfs, output, translator, cross, '1.2.1', shared)
            self.assertEqual(manifest['shaders'], len(expected))
            self.assertEqual({r['variant'].split('-')[0] for r in requests}, set(expected))
            for request in requests:
                key = request['variant'].split('-')[0]
                self.assertIs(request['early_fragment_tests'], expected[key])
                variants = json.loads((output/(key+'.variants')).read_text())
                self.assertEqual(variants, [dict(variant=request['variant'], deps=seed.ANSWERS)])

    def test_reuse_requires_the_current_early_fragment_tests_policy_even_with_matching_build_hashes(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            identity = dict(schema=1, chain=seed.CHAIN, version='1.2.1', source_romfs_sha256='c'*64,
                            translator_sha256='d'*64, preparation_sha256='e'*64,
                            early_fragment_tests_policy=seed.EARLY_FRAGMENT_TESTS_POLICY)
            scope = seed.shader_key_scope(['a'*64], '1.2.1', 'c'*64)
            (root/'shader-keys.json').write_text(json.dumps(scope))
            for policy in [None, 'nvn-control-0x718-v0', seed.EARLY_FRAGMENT_TESTS_POLICY]:
                manifest = dict(**identity, shaders=1,
                                files={'shader-keys.json': seed.digest((root/'shader-keys.json').read_bytes())})
                if policy is None:
                    del manifest['early_fragment_tests_policy']
                else:
                    manifest['early_fragment_tests_policy'] = policy
                (root/'seed-manifest.json').write_text(json.dumps(manifest))
                with self.subTest(policy=policy):
                    self.assertEqual(seed.reusable_seed(root, identity, min_shaders=1),
                                     manifest if policy == seed.EARLY_FRAGMENT_TESTS_POLICY else None)

    def test_seed_dedup_keeps_a_usable_representative_not_every_input_hash(self):
        # Device fills hash all bound answers, while a list row carries only the translator's
        # dependencies. Neither a different input hash nor a discarded duplicate means lost coverage.
        key, stage, control, code = next(seed.bnsh_shaders(bnsh()))
        requests = []

        class Service:
            def __init__(self, *args, **kwargs):
                self.stdin = self.stdout = self

            def write(self, line):
                request = json.loads(line)
                requests.append(request)
                base = Path(request['code']).with_suffix('')
                deps = dict(tex={'0': request['tex'].get('0', '2d')}, img={}, attr={}, fout={})
                base.with_name(base.name+'_b.metal').write_text('// synthetic shader')
                base.with_name(base.name+'_b.json').write_text(json.dumps(
                    dict(chain=seed.CHAIN, stage=request['stage'], deps=deps)))
                model = {'vs': 0, 'ps': 4, 'cs': 5}[request['stage']]
                base.with_name(base.name+'.spv').write_bytes(
                    struct.pack('<9I', 0x07230203, 0x00010300, 0, 2, 0, (4 << 16) | 15, model, 1, 0))
                base.with_name(base.name+'.res.json').write_text('{}')

            def flush(self): pass
            def close(self): pass
            def wait(self): return 0
            def readline(self): return '{"ok":true}\n'

        states = [
            dict(tex={'0': '2d'}, img={}, attr={}, fout={}),
            dict(tex={'0': '2d', '7': 'cube'}, img={}, attr={}, fout={}),
            dict(tex={'0': 'cube'}, img={}, attr={}, fout={}),
        ]
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); romfs = root/'romfs'; romfs.mkdir()
            (romfs/'index.json').write_text(json.dumps(dict(romfs_sha256='c'*64)))
            translator, cross, shared = [root/name for name in ['translator', 'spirv-cross', 'list.tsv']]
            for path in (translator, cross): path.write_text('')
            shared.write_text(''.join(f'V\t{key}\t'+json.dumps(s, sort_keys=True, separators=(',', ':'))+'\n'
                                      for s in states))
            output = root/'seed'
            with mock.patch.dict(seed.GAMES['totk'], min_shaders=1), \
                    mock.patch.object(seed, 'inventory', return_value=(
                        {}, {key: (stage, control, code, 'Shader/test')}, {key}, [])), \
                    mock.patch.object(seed, 'source_reader', return_value=lambda _: None), \
                    mock.patch.object(seed.os, 'cpu_count', return_value=1), \
                    mock.patch.object(seed.subprocess, 'Popen', side_effect=Service):
                manifest = seed.prepare(romfs, output, translator, cross, 'v143', shared, keep_spirv=True)
            variants = json.loads((output/(key+'.variants')).read_text())
            self.assertEqual(manifest['shaders'], 2)
            self.assertEqual(len(requests), 3)   # exact dependency input skips; redundant full input translates then dedups
            self.assertEqual(manifest['skipped'], [])
            self.assertEqual({json.dumps(v['deps'], sort_keys=True) for v in variants},
                             {json.dumps(s, sort_keys=True) for s in (states[0], states[2])})
            self.assertIn(key+'-'+seed.SUFFIX, {v['variant'] for v in variants})
            redundant = key+'-'+seed.suffix(states[1])
            self.assertNotIn(redundant, {v['variant'] for v in variants})
            self.assertFalse((output/(redundant+'.spv')).exists())
            for variant in variants:
                for ext in ('.spv', '.res.json', '_b.json'):
                    self.assertTrue((output/(variant['variant']+ext)).is_file())
                    self.assertIn(variant['variant']+ext, manifest['files'])


class RetainedVulkanSeed(unittest.TestCase):
    def test_exact_compute_names_survive_equivalent_dependency_deduplication(self):
        keys = ['3c8e9ba061067c079f1332989ac84d6412d15c066d7c17ccef9fe07e061249fd',
                'bc58d5dec860c367e3386a8eb93db6525747ee043531cbb5bfc929d3ba494a1a',
                'c223a938382abd73353fe76cc5df87937a350aa88623c0000cd643870b08461b']
        deps = dict(attr={}, fout={}, img={}, tex={'6': '2d_depth'})
        control = bytearray(0x880)
        struct.pack_into('<II', control, 0x700, 0, 0x100)
        struct.pack_into('<III', control, 0x748, 32, 1, 1)
        spirv = struct.pack('<9I', 0x07230203, 0x00010300, 0, 2, 0, (4 << 16) | 15, 5, 1, 0)

        class Service:
            def __init__(self, *args, **kwargs): self.stdin = self.stdout = self
            def write(self, line):
                request = json.loads(line)
                base = Path(request['code']).with_suffix('')
                base.with_suffix('.spv').write_bytes(spirv)
                base.with_name(base.name + '.res.json').write_text('{}')
                base.with_name(base.name + '_b.metal').write_text('// synthetic compute')
                base.with_name(base.name + '_b.json').write_text(json.dumps(
                    dict(chain=seed.CHAIN, stage='cs', deps=deps)))
            def flush(self): pass
            def close(self): pass
            def wait(self): return 0
            def readline(self): return '{"ok":true}\n'

        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root/'index.json').write_text(json.dumps(dict(romfs_sha256='c'*64)))
            binary = root/'translator'; binary.write_bytes(b'pinned translator')
            previous = root/'previous'; previous.mkdir()
            for key in keys:
                variant = key + '-de54a198dbf6'
                (previous/(key+'.vv')).write_text(f'{variant}\ttex:6=2d_depth\n')
                (previous/(variant+'.spv')).write_bytes(spirv)
                (previous/(variant+'.vk')).write_text('vk 1\nstage 5\n')
            shaders = {key: ('cs', bytes(control), bytes(0x100), 'Shader/synthetic') for key in keys}
            with mock.patch.dict(seed.GAMES['totk'], min_shaders=1), \
                    mock.patch.object(seed, 'inventory', return_value=({}, shaders, set(keys), [])), \
                    mock.patch.object(seed, 'source_reader', return_value=lambda _: None), \
                    mock.patch.object(seed.os, 'cpu_count', return_value=1), \
                    mock.patch.object(seed.subprocess, 'Popen', side_effect=Service):
                output = root/'prepared'
                report = seed.prepare(root, output, binary, binary, 'v143', root/'missing-list',
                                      keep_spirv=True, retain_vulkan_seed=previous)
            self.assertEqual(report['retained_variants'], 3)
            self.assertEqual(report['shaders'], 6)
            self.assertEqual(report['retained_outside_inventory'], [])
            self.assertEqual(report['translator_sha256'], seed.digest(binary.read_bytes()))
            for key in keys:
                variants = json.loads((output/(key+'.variants')).read_text())
                self.assertEqual({entry['variant'] for entry in variants},
                                 {key+'-'+seed.SUFFIX, key+'-de54a198dbf6'})
                self.assertTrue((output/(key+'-de54a198dbf6.spv')).is_file())

    def test_retained_fragment_name_keeps_historical_early_mode_while_new_name_uses_control(self):
        key, alias = 'd'*64, 'd'*64+'-'+'e'*12
        control = bytearray(0x880); control[0x718] = 1
        struct.pack_into('<II', control, 0x700, 0, 0x100)
        spirv = struct.pack('<9I', 0x07230203, 0x00010300, 0, 2, 0, (4 << 16) | 15, 4, 1, 0)
        requests = {}
        class Service:
            def __init__(self, *args, **kwargs): self.stdin = self.stdout = self
            def write(self, line):
                request = json.loads(line); requests[request['variant']] = request['early_fragment_tests']
                base = Path(request['code']).with_suffix('')
                mode = struct.pack('<3I', (3 << 16) | 16, 1, 9) if request['early_fragment_tests'] else b''
                base.with_suffix('.spv').write_bytes(spirv + mode)
                base.with_name(base.name + '.res.json').write_text('{}')
                base.with_name(base.name + '_b.metal').write_text('// synthetic fragment')
                base.with_name(base.name + '_b.json').write_text(json.dumps(
                    dict(chain=seed.CHAIN, stage='ps', deps=seed.ANSWERS)))
            def flush(self): pass
            def close(self): pass
            def wait(self): return 0
            def readline(self): return '{"ok":true}\n'
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root/'index.json').write_text(json.dumps(dict(romfs_sha256='c'*64)))
            binary = root/'translator'; binary.write_bytes(b'pinned translator')
            previous = root/'previous'; previous.mkdir()
            (previous/(key+'.vv')).write_text(f'{alias}\t\n')
            (previous/(alias+'.spv')).write_bytes(spirv)
            (previous/(alias+'.vk')).write_text('vk 1\nstage 1\n')
            with mock.patch.dict(seed.GAMES['totk'], min_shaders=1), \
                    mock.patch.object(seed, 'inventory', return_value=(
                        {}, {key: ('ps', bytes(control), bytes(0x100), 'Shader/synthetic')}, {key}, [])), \
                    mock.patch.object(seed, 'source_reader', return_value=lambda _: None), \
                    mock.patch.object(seed.os, 'cpu_count', return_value=1), \
                    mock.patch.object(seed.subprocess, 'Popen', side_effect=Service):
                output = root/'prepared'
                seed.prepare(root, output, binary, binary, 'v143', root/'missing-list',
                             keep_spirv=True, retain_vulkan_seed=previous)
            self.assertEqual(requests, {key+'-'+seed.SUFFIX: True, alias: False})
            self.assertEqual((output/(alias+'.spv')).read_bytes(), spirv)

    def test_retained_index_is_closed_and_rejects_conflicting_dependencies(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); key = 'a'*64; variant = key+'-'+'b'*12
            index = root/(key+'.vv')
            index.write_text(f'{variant}\timg:2=2d/rgba8;tex:6=2darray_depth\n')
            with self.assertRaisesRegex(ValueError, 'dangling'):
                seed.retained_vulkan_requests(root)
            spirv = struct.pack('<9I', 0x07230203, 0x00010300, 0, 2, 0, (4 << 16) | 15, 5, 1, 0)
            (root/(variant+'.spv')).write_bytes(spirv)
            (root/(variant+'.vk')).write_text('vk 1\nstage 5\n')
            request = seed.retained_vulkan_requests(root)[variant]
            self.assertEqual(request['answers']['img'], {'2': dict(type='2d', fmt='rgba8')})
            self.assertEqual(request['answers']['tex'], {'6': '2darray_depth'})
            self.assertFalse(request['early_fragment_tests'])
            (root/(variant+'.spv')).write_bytes(spirv + struct.pack('<3I', (3 << 16) | 16, 1, 9))
            self.assertTrue(seed.retained_vulkan_requests(root)[variant]['early_fragment_tests'])
            index.write_text(index.read_text()+f'{variant}\ttex:6=cube\n')
            with self.assertRaisesRegex(ValueError, 'conflicting'):
                seed.retained_vulkan_requests(root)


class ShaderKeyScope(unittest.TestCase):
    def test_scope_has_sorted_unique_raw_keys_and_game_identity(self):
        scope = seed.shader_key_scope(['b'*64, 'a'*64, 'b'*64], '1.2.1', 'c'*64)
        self.assertEqual(scope, dict(schema=1, chain=seed.CHAIN, version='1.2.1',
                                     source_romfs_sha256='c'*64, keys=['a'*64, 'b'*64]))

    def test_invalid_identity_or_variant_names_cannot_become_raw_keys(self):
        for keys in [[], 'a'*64, ['a'*63], ['A'*64], ['g'*64], ['a'*64+'-123456789abc'], [None]]:
            with self.subTest(keys=keys), self.assertRaises(ValueError):
                seed.shader_key_scope(keys, '1.2.1', 'c'*64)
        for kwargs in [dict(chain='other'), dict(source_romfs_sha256='unknown'), dict(version='')]:
            values = dict(keys=['a'*64], version='1.2.1', source_romfs_sha256='c'*64)
            values.update(kwargs)
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                seed.shader_key_scope(**values)

    def test_preparation_keeps_untranslated_and_located_keys_and_rebuilds_stale_seed(self):
        # A tiny service prepares one startup shader. The other inventory shader and verified locator need no startup
        # variant but still belong to this game; filtering by the translated seed would incorrectly lose their cache.
        class Service:
            def __init__(self, *args, **kwargs):
                self.stdin = self.stdout = self

            def write(self, line):
                request = json.loads(line)
                base = Path(request['code']).with_suffix('')
                base.with_name(base.name+'_b.metal').write_text('// synthetic shader')
                base.with_name(base.name+'_b.json').write_text(json.dumps(
                    dict(chain=seed.CHAIN, stage=request['stage'], deps=seed.ANSWERS)))

            def flush(self): pass
            def close(self): pass
            def wait(self): return 0
            def readline(self): return '{"ok":true}\n'

        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); romfs = root/'romfs'; romfs.mkdir()
            (romfs/'index.json').write_text(json.dumps(dict(romfs_sha256='c'*64)))
            translator, cross, shared = [root/name for name in ['translator', 'spirv-cross', 'list.tsv']]
            for path in [translator, cross, shared]: path.write_text('')
            rows = list(seed.bnsh_shaders(bnsh()))
            shaders = {key: (stage, control, code, 'Shader/test') for key, stage, control, code in rows[:2]}
            extra = {rows[2][0]: (*rows[2][1:], 'module:main')}
            output = root/'seed'
            with mock.patch.object(seed, 'MIN_SHADERS', 1), mock.patch.dict(seed.GAMES['totk'], min_shaders=1), \
                    mock.patch.object(seed, 'inventory', return_value=({}, shaders, {rows[0][0]}, [])) as inventory, \
                    mock.patch.object(seed, 'source_reader', return_value=lambda _: None), \
                    mock.patch.object(seed, 'located', return_value=extra), \
                    mock.patch.object(seed.subprocess, 'Popen', side_effect=Service):
                manifest = seed.prepare(romfs, output, translator, cross, '1.2.1', shared)
                self.assertEqual(manifest['early_fragment_tests_policy'], 'nvn-control-0x718-v1')
                scope = json.loads((output/'shader-keys.json').read_text())
                self.assertEqual(scope['keys'], sorted(row[0] for row in rows))
                self.assertEqual((manifest['shaders'], manifest['discovered'], manifest['located']), (1, 3, 1))
                self.assertEqual(manifest['files']['shader-keys.json'], seed.digest((output/'shader-keys.json').read_bytes()))
                self.assertEqual(seed.prepare(romfs, output, translator, cross, '1.2.1', shared), manifest)
                self.assertEqual(inventory.call_count, 1)
                # Keep every build hash and output file intact: the semantic policy alone must force preparation.
                for expected_calls, policy in enumerate([None, 'nvn-control-0x718-v0'], start=2):
                    stale = dict(manifest)
                    if policy is None:
                        del stale['early_fragment_tests_policy']
                    else:
                        stale['early_fragment_tests_policy'] = policy
                    (output/'seed-manifest.json').write_text(json.dumps(stale))
                    manifest = seed.prepare(romfs, output, translator, cross, '1.2.1', shared)
                    self.assertEqual(manifest['early_fragment_tests_policy'], 'nvn-control-0x718-v1')
                    self.assertEqual(inventory.call_count, expected_calls)
                    self.assertEqual(seed.prepare(romfs, output, translator, cross, '1.2.1', shared), manifest)
                    self.assertEqual(inventory.call_count, expected_calls)
                # A pre-metadata seed may have all successful translations and matching build hashes. It must still
                # regenerate its inventory instead of treating that smaller list as the game's full scope.
                del manifest['files']['shader-keys.json']
                (output/'seed-manifest.json').write_text(json.dumps(manifest))
                seed.prepare(romfs, output, translator, cross, '1.2.1', shared)
                self.assertEqual(inventory.call_count, 4)
                (output/'shader-keys.json').unlink()
                seed.prepare(romfs, output, translator, cross, '1.2.1', shared)
                self.assertEqual(inventory.call_count, 5)

    def test_reuse_rejects_wrong_scope_identity_even_with_a_matching_file_hash(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            identity = dict(schema=1, chain=seed.CHAIN, version='1.2.1', source_romfs_sha256='c'*64)
            good = seed.shader_key_scope(['a'*64], '1.2.1', 'c'*64)
            for changes in [{}, dict(chain='other'), dict(version='1.0.0'), dict(source_romfs_sha256='d'*64),
                            dict(keys=['a'*64+'-123456789abc']), dict(keys=[])]:
                scope = {**good, **changes}
                (root/'shader-keys.json').write_text(json.dumps(scope))
                manifest = dict(**identity, shaders=seed.MIN_SHADERS,
                                files={'shader-keys.json': seed.digest((root/'shader-keys.json').read_bytes())})
                (root/'seed-manifest.json').write_text(json.dumps(manifest))
                with self.subTest(changes=changes):
                    self.assertEqual(seed.reusable_seed(root, identity), None if changes else manifest)


@unittest.skipUnless(shutil.which('xcrun'), 'native Foundation importer is Mac-only')
class ShaderImport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        source = cls.root/'probe.m'
        source.write_text('''#import "totk_shader_seed.h"
int main(int argc, const char **argv) { @autoreleasepool {
 NSError *error = nil; NSDictionary *r = nil;
 if (argc == 4 && !strcmp(argv[1], "--fill")) r = totk_fill_import(@(argv[2]), @(argv[3]));
 else if (argc == 3) r = totk_seed_import(@(argv[1]), @(argv[2]), &error);
 else return 2;
 if (!r) { fprintf(stderr, "%s\\n", error.localizedDescription.UTF8String); return 1; }
 NSData *d = [NSJSONSerialization dataWithJSONObject:r options:0 error:nil];
 puts([[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding].UTF8String); return 0;
} }
''')
        cls.binary = cls.root/'probe'
        subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                        '-Wno-deprecated-declarations', '-I'+str(ROOT/'games/totk/third_party/d2r_gfx/gfx/nvn'),
                        str(source), '-framework', 'Foundation', '-o', str(cls.binary)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def fixture(self, root):
        source, cache = root/'seed', root/'cache'
        source.mkdir(); cache.mkdir()
        key, variant = 'a'*64, 'a'*64+'-'+'b'*12
        files = {variant+'_b.metal': b'// synthetic shader', variant+'_b.json': b'{}',
                 key+'.variants': json.dumps([dict(variant=variant, deps={})]).encode(),
                 'pso_manifest.tsv': ('S\t'+variant+'_b\t0\n').encode()}
        for name, data in files.items():
            (source/name).write_bytes(data)
        manifest = dict(schema=1, chain=seed.CHAIN, shaders=1,
                        files={n: hashlib.sha256(d).hexdigest() for n, d in files.items()})
        (source/'seed-manifest.json').write_text(json.dumps(manifest))
        return source, cache, key, variant

    def run_probe(self, source, cache, good=True, fill=False):
        r = subprocess.run([str(self.binary)]+(['--fill'] if fill else [])+[str(source), str(cache)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0 if good else 1, r.stderr)
        return json.loads(r.stdout) if good else r.stderr

    def test_first_import_reuse_and_preservation_of_user_variants(self):
        with tempfile.TemporaryDirectory() as folder:
            source, cache, key, variant = self.fixture(Path(folder))
            user = dict(variant=key+'-'+'c'*12, deps={'tex': {'0': '2d_array'}})
            (cache/(key+'.variants')).write_text(json.dumps([user]))
            (cache/'unrelated.txt').write_text('keep')
            r = self.run_probe(source, cache)
            self.assertGreater(r['imported_files'], 0)
            self.assertEqual(json.loads((cache/(key+'.variants')).read_text()), [user, dict(variant=variant, deps={})])
            self.assertEqual(self.run_probe(source, cache)['imported_files'], 0)
            (cache/(variant+'_b.metal')).unlink()
            self.assertGreater(self.run_probe(source, cache)['imported_files'], 0)
            self.assertEqual((cache/'unrelated.txt').read_text(), 'keep')

    def test_reprepared_seed_with_the_same_files_is_not_checked_again(self):
        # Packaging again writes a new preparation time and script hash into the manifest; the files stay the same.
        with tempfile.TemporaryDirectory() as folder:
            source, cache, key, variant = self.fixture(Path(folder))
            self.run_probe(source, cache)
            p = source/'seed-manifest.json'; m = json.loads(p.read_text())
            m['seconds'] = 12.5; m['preparation_sha256'] = 'f'*64
            p.write_text(json.dumps(m))
            (source/(variant+'_b.metal')).write_text('corrupt')   # only a check of the sources would notice
            self.assertEqual(self.run_probe(source, cache)['imported_files'], 0)

    def test_a_changed_file_table_is_checked_again(self):
        with tempfile.TemporaryDirectory() as folder:
            source, cache, key, variant = self.fixture(Path(folder))
            self.run_probe(source, cache)
            p = source/'seed-manifest.json'; m = json.loads(p.read_text())
            extra = key+'-'+'e'*12+'_b.metal'
            (source/extra).write_bytes(b'// new shader')
            m['files'][extra] = hashlib.sha256(b'// new shader').hexdigest(); m['shaders'] = 2
            p.write_text(json.dumps(m))
            (source/(variant+'_b.metal')).write_text('corrupt')   # caught because every source is checked again
            self.run_probe(source, cache, good=False)

    def test_wrong_chain_corruption_and_path_traversal_are_rejected(self):
        for mutation in ['chain', 'hash', 'path']:
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as folder:
                source, cache, key, variant = self.fixture(Path(folder))
                p = source/'seed-manifest.json'; m = json.loads(p.read_text())
                if mutation == 'chain': m['chain'] = 'other-chain'
                elif mutation == 'hash': (source/(variant+'_b.metal')).write_text('corrupt')
                else: m['files']['../escape'] = '0'*64
                p.write_text(json.dumps(m))
                self.run_probe(source, cache, good=False)
                self.assertEqual(list(cache.iterdir()), [])

    def test_fill_packs_merge_without_touching_the_seed_receipt(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source, cache, key, variant = self.fixture(root)
            self.run_probe(source, cache)
            receipt = (cache/'seed-installed.json').read_text()
            fills = root/'fills'; fills.mkdir()
            pack = fills/'20261018-1'; pack.mkdir()
            filled = key+'-'+'d'*12
            files = {filled+'_b.metal': b'// filled', filled+'_b.json': b'{}', filled+'_b.metallib': b'lib',
                     key+'.variants': json.dumps([dict(variant=filled, deps={'tex': {'1': 'cube'}})]).encode()}
            for name, data in files.items():
                (pack/name).write_bytes(data)
            (pack/'seed-manifest.json').write_text(json.dumps(dict(schema=1, chain=seed.CHAIN, shaders=1,
                files={n: hashlib.sha256(d).hexdigest() for n, d in files.items()})))
            r = self.run_probe(fills, cache, fill=True)
            self.assertEqual((r['packs'], r['rejected']), (1, []))
            self.assertEqual([v['variant'] for v in json.loads((cache/(key+'.variants')).read_text())], [variant, filled])
            self.assertEqual((cache/(filled+'_b.metallib')).read_bytes(), b'lib')
            self.assertEqual((cache/'seed-installed.json').read_text(), receipt)
            self.assertEqual(list(fills.iterdir()), [])

    def test_a_rebuilt_metallib_replaces_the_cached_one(self):
        # iOS metallibs once compiled with Metal's default fast math must be replaced by the safe-math rebuild; the
        # sources next to them are still kept as they are.
        with tempfile.TemporaryDirectory() as folder:
            source, cache, key, variant = self.fixture(Path(folder))
            lib = variant+'_b.metallib'
            (cache/lib).write_bytes(b'fast-math lib'); (cache/(variant+'_b.metal')).write_bytes(b'// collected here')
            (source/lib).write_bytes(b'safe-math lib')
            p = source/'seed-manifest.json'; m = json.loads(p.read_text())
            m['files'][lib] = hashlib.sha256(b'safe-math lib').hexdigest(); p.write_text(json.dumps(m))
            self.run_probe(source, cache)
            self.assertEqual((cache/lib).read_bytes(), b'safe-math lib')
            self.assertEqual((cache/(variant+'_b.metal')).read_bytes(), b'// collected here')

    def test_incomplete_fill_pack_is_dropped_without_changing_the_cache(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source, cache, key, variant = self.fixture(root)
            fills = root/'fills'
            shutil.copytree(source, fills/'partial')
            (fills/'partial'/(variant+'_b.json')).unlink()   # an interrupted push
            r = self.run_probe(fills, cache, fill=True)
            self.assertEqual((r['packs'], len(r['rejected'])), (0, 1))
            self.assertEqual(list(cache.iterdir()), [])
            self.assertEqual(list(fills.iterdir()), [])


if __name__ == '__main__':
    unittest.main()
