#!/usr/bin/env python3
"""Dependency-free SPIR-V layout tests: python3 -m unittest discover -s games/totk/android -p test_vk_reflect.py."""
import struct
import unittest

import vk_reflect


def module(*instructions):
    """Synthetic declaration-only modules exercise reflection, not SPIR-V validation."""
    words = [0x07230203, 0x00010300, 0, 128, 0]
    for op, *args in instructions:
        words.extend([((len(args) + 1) << 16) | op, *args])
    return struct.pack(f'<{len(words)}I', *words)


def storage_reflection(*layout, storage=12, descriptors=1, block_decoration=2):
    # Shared types: float, uint, int, vec3, mat2x3. The block, pointer and variable are 20, 21, 22.
    name = struct.unpack('<2I', b'fp_s0\0\0\0')
    declarations = [(15, 4, 127, 0), (5, 22, *name), (71, 22, 33, 0), (71, 20, block_decoration),
                    (22, 1, 32), (21, 2, 32, 0), (21, 3, 32, 1), (23, 4, 1, 3), (24, 5, 4, 2), *layout]
    pointee = 20
    if descriptors != 1:
        declarations.extend([(43, 2, 30, descriptors), (28, 31, 20, 30)])
        pointee = 31
    declarations.extend([(32, 21, storage, pointee), (59, 21, 22, storage)])
    return vk_reflect.reflect(module(*declarations), {'sbufs': [{'binding': 0, 'cb_slot': 0, 'cb_offset': 0x510 // 4}]}, 1)


def storage_extent(*layout, **kwargs):
    return storage_reflection(*layout, **kwargs)['binds'][0]['extent']


class StorageExtentTests(unittest.TestCase):
    def test_execution_model_must_match_reflection_stage(self):
        with self.assertRaisesRegex(ValueError, 'execution model 0 disagrees with NVN stage 1'):
            vk_reflect.reflect(module((15, 0, 127, 0)), {}, 1)
        with self.assertRaisesRegex(ValueError, 'execution model None'):
            vk_reflect.reflect(module(), {}, 1)

    def test_struct_offsets_and_vector_tail(self):
        self.assertEqual(storage_extent((30, 20, 1, 4), (72, 20, 0, 35, 0), (72, 20, 1, 35, 16)), 28)

    def test_nested_struct_array_includes_stride_padding(self):
        self.assertEqual(storage_extent((30, 6, 1, 4), (72, 6, 0, 35, 0), (72, 6, 1, 35, 16),
                                        (43, 2, 7, 3), (28, 8, 6, 7), (71, 8, 6, 32),
                                        (30, 20, 8), (72, 20, 0, 35, 64)), 160)

    def test_nested_fixed_arrays(self):
        self.assertEqual(storage_extent((43, 2, 6, 3), (28, 7, 1, 6), (71, 7, 6, 16),
                                        (43, 2, 8, 2), (28, 9, 7, 8), (71, 9, 6, 64),
                                        (30, 20, 9), (72, 20, 0, 35, 0)), 128)

    def test_matrix_major_order_and_stride(self):
        for major, expected in ((5, 48), (4, 64)):
            with self.subTest(major=major):
                self.assertEqual(storage_extent((30, 20, 5), (72, 20, 0, 35, 16),
                                                (72, 20, 0, 7, 16), (72, 20, 0, major)), expected)

    def test_array_of_matrices_uses_member_layout(self):
        self.assertEqual(storage_extent((43, 2, 6, 3), (28, 7, 5, 6), (71, 7, 6, 64),
                                        (30, 20, 7), (72, 20, 0, 35, 32),
                                        (72, 20, 0, 7, 16), (72, 20, 0, 4)), 224)

    def test_nested_struct_matrix_uses_its_own_layout(self):
        self.assertEqual(storage_extent((30, 6, 5), (72, 6, 0, 35, 16), (72, 6, 0, 7, 16), (72, 6, 0, 4),
                                        (30, 20, 6), (72, 20, 0, 35, 32)), 96)

    def test_scalar_widths(self):
        for op, width in ((21, 8), (21, 16), (21, 64), (22, 16), (22, 64)):
            with self.subTest(op=op, width=width):
                scalar = (op, 6, width, 0) if op == 21 else (op, 6, width)
                self.assertEqual(storage_extent(scalar, (30, 20, 6), (72, 20, 0, 35, 8)), 8 + width // 8)

    def test_descriptor_array_extent_is_per_buffer(self):
        r = storage_reflection((30, 20, 4), (72, 20, 0, 35, 0), descriptors=3)
        self.assertEqual(r['binds'][0]['count'], 3)
        self.assertEqual(r['binds'][0]['extent'], 12)

    def test_legacy_uniform_storage_block(self):
        self.assertEqual(storage_extent((30, 20, 4), (72, 20, 0, 35, 0), storage=2, block_decoration=3), 12)

    def test_uniform_block_is_not_a_storage_extent(self):
        self.assertEqual(storage_extent((30, 20, 4), (72, 20, 0, 35, 0), storage=2), 0)

    def test_runtime_array_never_returns_static_prefix(self):
        self.assertEqual(storage_extent((29, 6, 1), (71, 6, 6, 4), (30, 20, 4, 6),
                                        (72, 20, 0, 35, 0), (72, 20, 1, 35, 16)), 0)

    def test_nested_unknown_member_invalidates_entire_block(self):
        self.assertEqual(storage_extent((29, 6, 1), (71, 6, 6, 4), (30, 7, 6), (72, 7, 0, 35, 0),
                                        (30, 20, 4, 7), (72, 20, 0, 35, 0), (72, 20, 1, 35, 16)), 0)

    def test_unsupported_members_never_return_static_prefix(self):
        for declaration in ((20, 6), (32, 6, 12, 1), (21, 6, 128, 0)):
            with self.subTest(declaration=declaration):
                self.assertEqual(storage_extent(declaration, (30, 20, 4, 6),
                                                (72, 20, 0, 35, 0), (72, 20, 1, 35, 16)), 0)

    def test_all_struct_members_need_offsets(self):
        self.assertEqual(storage_extent((30, 20, 4, 1), (72, 20, 0, 35, 0)), 0)

    def test_array_needs_known_element_and_stride(self):
        for element, stride in ((1, None), (4, 4), (6, 16)):
            with self.subTest(element=element, stride=stride):
                layout = [(20, 6), (43, 2, 7, 3), (28, 8, element, 7), (30, 20, 8), (72, 20, 0, 35, 0)]
                if stride is not None:
                    layout.append((71, 8, 6, stride))
                self.assertEqual(storage_extent(*layout), 0)

    def test_matrix_requires_supported_explicit_layout(self):
        for layout in ([], [(72, 20, 0, 5)], [(72, 20, 0, 7, 16)],
                       [(72, 20, 0, 7, 16), (72, 20, 0, 4), (72, 20, 0, 5)],
                       [(72, 20, 0, 7, 4), (72, 20, 0, 5)]):
            with self.subTest(layout=layout):
                self.assertEqual(storage_extent((30, 20, 5), (72, 20, 0, 35, 0), *layout), 0)

    def test_array_length_must_be_positive_literal_integer(self):
        for constant in ((43, 2, 6, 0), (43, 3, 6, 0xffffffff), (50, 2, 6, 3), (43, 1, 6, 0x40400000)):
            with self.subTest(constant=constant):
                self.assertEqual(storage_extent(constant, (28, 7, 1, 6), (71, 7, 6, 4),
                                                (30, 20, 7), (72, 20, 0, 35, 0)), 0)

    def test_64_bit_array_length_is_not_truncated(self):
        self.assertEqual(storage_extent((21, 6, 64, 0), (43, 6, 7, 1, 1), (28, 8, 1, 7), (71, 8, 6, 4),
                                        (30, 20, 8), (72, 20, 0, 35, 0)), ((1 << 32) + 1) * 4)

    def test_extent_overflow_is_unknown(self):
        for lo, hi, offset in ((0xffffffff, 0xffffffff, 0), (0xffffffff, 0x3fffffff, 16)):
            with self.subTest(lo=lo, hi=hi, offset=offset):
                self.assertEqual(storage_extent((21, 6, 64, 0), (43, 6, 7, lo, hi), (28, 8, 1, 7), (71, 8, 6, 4),
                                                (30, 20, 8), (72, 20, 0, 35, offset)), 0)

    def test_metadata_appends_extent_without_changing_old_fields(self):
        r = storage_reflection((30, 20, 4), (72, 20, 0, 35, 0))
        line = next(line for line in vk_reflect.vk_text(r, 1, '').splitlines() if line.startswith('b '))
        self.assertEqual(line, 'b 0 1 7 6 0 -1 0 0 1 0 12')
        del r['binds'][0]['extent']
        legacy = next(line for line in vk_reflect.vk_text(r, 1, '').splitlines() if line.startswith('b '))
        self.assertEqual(legacy.rsplit(' ', 1), [line.rsplit(' ', 1)[0], '0'])

    def test_truncated_instruction_rejected(self):
        data = module((30, 20, 1))[:-4]
        with self.assertRaises(ValueError):
            vk_reflect.Spirv(data)


if __name__ == '__main__':
    unittest.main()
