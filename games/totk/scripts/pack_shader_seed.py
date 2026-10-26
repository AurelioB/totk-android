#!/usr/bin/env python3
"""Pack a loose Vulkan shader seed without changing its files or cache identity.

    python3 games/totk/scripts/pack_shader_seed.py SEED [--output PATH]

The default output is SEED/seed.vkpack. Only immediate, regular .spv/.vk/.vv
files are included; payload symlinks and unsafe/non-ASCII basenames are errors.
Other files (including seed-id.txt) stay separate. Copy an existing seed-id.txt
alongside the pack when deploying it; do not regenerate it just for packing.

Format v1 is little-endian: a 32-byte <8sIIQQ> header (TOTKVKP1, version 1,
record size 104, count, total file size), followed by sorted <84sIQQ> records
(NUL-padded basename, reserved zero, absolute offset, length). Payload offsets
are 8-byte aligned, padding is zero, and no timestamps are stored. Publication
uses a temporary file beside the output and one atomic replacement. Stop any
translator writing the source directory before packing it.
"""
import argparse
import os
from pathlib import Path
import re
import stat
import struct
import tempfile


MAGIC = b'TOTKVKP1'
HEADER = struct.Struct('<8sIIQQ')
RECORD = struct.Struct('<84sIQQ')
EXTENSIONS = ('.spv', '.vk', '.vv')
BASENAME = re.compile(r'[A-Za-z0-9_.-]+', re.ASCII)
COPY_BYTES = 1024 * 1024


def _validate_name(name):
    if not BASENAME.fullmatch(name) or len(name) > 83:
        raise ValueError(f'invalid shader payload basename: {name!r}')
    extension = next((suffix for suffix in EXTENSIONS if name.endswith(suffix)), None)
    if extension is None or len(name) == len(extension):
        raise ValueError(f'invalid shader payload extension or empty stem: {name!r}')
    return name.encode('ascii')


def _identity(info):
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns


def _copy_payload(directory_fd, name, expected, output):
    # Open relative to the pinned directory without following a replacement
    # symlink; NONBLOCK also prevents a raced-in FIFO from hanging the converter.
    fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory_fd)
    with os.fdopen(fd, 'rb') as source:
        actual = os.fstat(source.fileno())
        if not stat.S_ISREG(actual.st_mode) or _identity(actual) != _identity(expected):
            raise ValueError(f'shader payload changed while packing: {name!r}')
        remaining = expected.st_size
        while remaining:
            data = source.read(min(remaining, COPY_BYTES))
            if not data:
                raise ValueError(f'short shader payload read: {name!r}')
            output.write(data)
            remaining -= len(data)
        if _identity(os.fstat(source.fileno())) != _identity(expected):
            raise ValueError(f'shader payload changed while packing: {name!r}')


def pack_seed(directory, output=None):
    """Atomically write a deterministic pack and return its Path; retain all inputs.

    Existing output is replaced only after a complete successful write. An
    explicit output must not replace a shader payload or another source file.
    The source directory is pinned and payload symlinks are never followed.
    """
    directory = Path(directory).expanduser().resolve(strict=True)
    output = Path(output).expanduser() if output is not None else directory / 'seed.vkpack'
    # Resolve parents only: resolving the final component would follow a symlink.
    output = output.parent.resolve(strict=True) / output.name
    if output.is_symlink():
        raise ValueError(f'output must not be a symlink: {output}')
    if output.parent == directory and (output.name.endswith(EXTENSIONS) or
                                      (output.name != 'seed.vkpack' and output.exists())):
        raise ValueError(f'output would replace a source file: {output}')
    directory_fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
    temporary = None
    try:
        entries = []
        with os.scandir(directory_fd) as files:
            for entry in files:
                if not entry.name.endswith(EXTENSIONS):
                    continue
                encoded = _validate_name(entry.name)
                info = entry.stat(follow_symlinks=False)
                if not stat.S_ISREG(info.st_mode):
                    raise ValueError(f'shader payload must be a regular file, not a symlink: {entry.name!r}')
                entries.append((encoded, entry.name, info))
        entries.sort(key=lambda row: row[0])
        records = []
        size = HEADER.size + len(entries) * RECORD.size
        for encoded, name, info in entries:
            if records and encoded == records[-1][0]:
                raise ValueError(f'duplicate shader payload basename: {name!r}')
            offset = (size + 7) & ~7
            size = offset + info.st_size
            if size >= 1 << 64:
                raise ValueError('shader pack exceeds the format size limit')
            records.append((encoded, name, info, offset))
        with tempfile.NamedTemporaryFile(mode='wb', dir=output.parent, prefix=f'.{output.name}.',
                                         suffix='.tmp', delete=False) as packed:
            temporary = Path(packed.name)
            packed.write(HEADER.pack(MAGIC, 1, RECORD.size, len(records), size))
            for encoded, _, info, offset in records:
                packed.write(RECORD.pack(encoded, 0, offset, info.st_size))
            for _, name, info, offset in records:
                packed.write(b'\0' * (offset - packed.tell()))
                _copy_payload(directory_fd, name, info, packed)
            packed.flush()
            os.fsync(packed.fileno())
        os.replace(temporary, output)
        temporary = None
    finally:
        os.close(directory_fd)
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return output


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('seed', metavar='SEED', type=Path, help='loose shader seed directory')
    parser.add_argument('--output', type=Path, help='output file (default: SEED/seed.vkpack)')
    args = parser.parse_args(argv)
    try:
        output = pack_seed(args.seed, args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, f'error: {error}\n')
    print(output)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
