"""No former guard words in the tree or the built libraries.

tests/former_guard_pairs.sha256 holds the SHA-256 of every pair of consecutive former guard words (two little-endian
u32, as the guard tables, veneer replays, fixtures and rewriter dispatch sequences held them at 9b5eac48, where the old
values can be recovered from git), minus pairs that also occur in generic compiled code. Pairs (64 bits), not single
words: a hash of one 32-bit word is as good as the word. Single words, records keyed by a public offset and mov/movk
constants need the player's main and are ContentAudit's guard_scan.py's job. Other titles (games/by3, games/botw) are
outside the published TotK tree and are not scanned; the retained rewriter (games/totk/third_party/d2r) is.

Scanned: every publishable tree file (hex literals in order, also every second one for {offset, word} records; hex blobs;
binary files as raw words) and, with TOTK_RELEASE_APK=<apk>, every library in the APK (raw words 1, 2 and 4 apart in its
non-executable sections, for packed records; hex text anywhere). A synthetic positive control proves each layout is found."""
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PAIRS = frozenset(bytes.fromhex(h) for h in (ROOT / 'tests/former_guard_pairs.sha256').read_text().split())
# Other titles (not part of the TotK publication; reviewed separately).
OTHER_TITLES = (b'games/by3/', b'games/botw/')
HEX_WORD = re.compile(rb'0x([0-9a-fA-F]{1,8})\b|\b([0-9a-fA-F]{8})\b')
HEX_BLOB = re.compile(rb'[0-9a-fA-F]{16,}')


def pair_hash(a, b):
    return hashlib.sha256(struct.pack('<II', a, b)).digest()


def scan_words(words, gaps, pairs):
    found = []
    for gap in gaps:
        for a, b in zip(words, words[gap:]):
            if a >= 0x1000000 and b >= 0x1000000 and pair_hash(a, b) in pairs:
                found.append((gap, a, b))
    return found


def scan_text(data, pairs):
    tokens = [int(m.group(1) or m.group(2), 16) for m in HEX_WORD.finditer(data)]
    found = scan_words(tokens, (1, 2), pairs)
    for blob in HEX_BLOB.findall(data):
        raw = bytes.fromhex(blob[:len(blob) // 8 * 8].decode())
        found += scan_words(struct.unpack(f'<{len(raw) // 4}I', raw), (1,), pairs)
    return found


def scan_binary(data, pairs, gaps=(1,)):
    data = data[:len(data) // 4 * 4]
    return scan_words(struct.unpack(f'<{len(data) // 4}I', data), gaps, pairs) + scan_text(data, pairs)


def data_sections(elf):
    """The bytes of an ELF64's non-executable sections (stored tables); .text is compiled code, where short instruction
    pairs recur by chance (prologues, mov/movk of a shared constant)."""
    if elf[:4] != b'\x7fELF' or elf[4] != 2:
        return elf
    shoff, = struct.unpack_from('<Q', elf, 0x28)
    entsize, count = struct.unpack_from('<HH', elf, 0x3a)
    out = []
    for i in range(count):
        _, kind, flags, _, offset, size = struct.unpack_from('<IIQQQQ', elf, shoff + i * entsize)
        if kind != 8 and not flags & 4:   # not NOBITS, not SHF_EXECINSTR
            out.append(elf[offset:offset + size] + bytes(-size % 4))
    return b''.join(out)


class FormerGuardWords(unittest.TestCase):
    def test_hash_list(self):
        self.assertGreaterEqual(len(PAIRS), 150)
        self.assertTrue(all(len(h) == 32 for h in PAIRS))

    def test_scanner_finds_each_layout(self):
        a, b = 0x5a17c0de, 0x6b28d1ef   # stand-ins, not game words
        pairs = PAIRS | {pair_hash(a, b)}
        self.assertTrue(scan_text(f'{{0x628c74, {a:#x}}}, {{0x628c78, {b:#x}}},'.encode(), pairs))   # records
        self.assertTrue(scan_text(f"'{a:08x}', '{b:08x}'".encode(), pairs))                         # bare words
        self.assertTrue(scan_text(struct.pack('<II', a, b).hex().encode(), pairs))                   # a hex blob
        self.assertTrue(scan_binary(b'\0' * 8 + struct.pack('<QII', 0x628c74, a, 0) + struct.pack('<QII', 0x628c78, b, 0),
                                    pairs, (1, 2, 4)))                                               # packed u64+u32 records
        self.assertFalse(scan_text(f'{a:#x} {a:#x}'.encode(), pairs))

    def test_tree_holds_no_former_pair(self):
        names = subprocess.run(['git', 'ls-files', '-z', '-co', '--exclude-standard'], cwd=ROOT, capture_output=True,
                               check=True).stdout.split(b'\0')
        hits = []
        for name in filter(None, names):
            path = ROOT / name.decode()
            if not path.is_file() or path.name == 'former_guard_pairs.sha256' or name.startswith(OTHER_TITLES):
                continue
            data = path.read_bytes()
            text = b'\0' not in data[:8192]
            found = scan_text(data, PAIRS) if text else scan_binary(data, PAIRS)
            hits += [(name.decode(), f) for f in found]
        self.assertEqual(hits, [])

    @unittest.skipUnless(os.environ.get('TOTK_RELEASE_APK'), 'set TOTK_RELEASE_APK to scan a built APK')
    def test_apk_libraries_hold_no_former_pair(self):
        hits = []
        with zipfile.ZipFile(os.environ['TOTK_RELEASE_APK']) as z:
            for name in z.namelist():
                if name.startswith('lib/') or name == 'classes.dex':
                    data = z.read(name)
                    hits += [(name, f) for f in scan_binary(data_sections(data), PAIRS, (1, 2, 4)) + scan_text(data, PAIRS)]
        self.assertEqual(hits, [])


if __name__ == '__main__':
    unittest.main()
