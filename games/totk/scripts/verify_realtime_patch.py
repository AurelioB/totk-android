#!/usr/bin/env python3
"""Execute the verified v1.0.0 frame-step instruction sites with Unicorn.

This checks the engine object's dt/ratio and the independent global ratio
writer for several measured frame intervals. The unpatched binary is a
negative control: it stays at the original interval-derived 60 Hz step.
"""
import hashlib
import json
import struct
import sys
from pathlib import Path

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_W1, UC_ARM64_REG_X0, UC_ARM64_REG_X19

ROOT = Path(__file__).resolve().parents[1]
from game_version_test import options, offsets, adrp_ldr
args=options();addr=offsets(args.version)
image = (ROOT / f"rewritten/{args.version}/main.patched.bin").read_bytes()
data_off = (len(image) + 0x3fff) & ~0x3fff
# The frame-step sites (1.0.0 offsets, mapped to the version). No game word is stored here: the player's image must hold
# the expected instructions, checked by the SHA-256 of the ten words (little-endian, in this order; runtime/code_guard.h).
SITES = (0x7a83f4, 0x7a83f8, 0x7a83fc, 0x7a8400, 0x7a840c, 0x7a8410, 0x8f68b8, 0x8f68bc, 0x8f68cc, 0x8f68d0)
SITES_SHA256 = "9695dcbd9df18e4e55ff326f4b051a85f7a08f9a2476c0acb35ebf679ae22d0a"
sites = [addr(a) for a in SITES]
fn=addr(0x7a83f4)-16
global_fn=addr(0x8f68b8)
global_got=adrp_ldr(image,global_fn+8,global_fn+12)
actual = b"".join(image[offset:offset + 4] for offset in sites)
if hashlib.sha256(actual).hexdigest() != SITES_SHA256:
    raise SystemExit(f"the frame-step instructions at main+{SITES[0]:#x}... are not the expected ones for {args.version}")


def adrp_x9(site, target):
    pages = (target >> 12) - (site >> 12)
    assert -(1 << 20) <= pages < (1 << 20)
    imm = pages & 0x1fffff
    return 0x90000009 | ((imm & 3) << 29) | ((imm >> 2) << 5)


replacements = [
    adrp_x9(addr(0x7a83f4), data_off), 0xbd400120, 0xbd400521,
    0xd503201f, 0xd503201f, 0xd503201f,
    adrp_x9(addr(0x8f68b8), data_off), 0xbd400120,
    0xd503201f, 0xd503201f,
]


def execute(seconds, patched):
    mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    for address in (fn & ~0xfff, global_fn & ~0xfff, data_off, global_got & ~0xfff, 0x100000):
        mu.mem_map(address, 0x4000)
    for address, size in ((fn, 0x38), (global_fn, 0x20)):
        mu.mem_write(address, image[address:address + size])
    if patched:
        for address, word in zip(sites, replacements):
            mu.mem_write(address, struct.pack("<I", word))
    mu.mem_write(data_off, struct.pack("<ff", seconds * 30, seconds))
    # The global-ratio writer resolves *(main+0x45666a0) to this float.
    mu.mem_write(global_got, struct.pack("<Q", 0x100200))
    mu.mem_write(0x10025c, struct.pack("<I", 1))
    mu.reg_write(UC_ARM64_REG_X0, 0x100000)
    mu.reg_write(UC_ARM64_REG_W1, 1)
    mu.reg_write(UC_ARM64_REG_SP, 0x103000)
    mu.emu_start(fn, fn+0x34, count=40)
    assert mu.reg_read(UC_ARM64_REG_PC) == fn+0x34
    ratio, dt = struct.unpack("<ff", mu.mem_read(0x100008, 8))
    mu.reg_write(UC_ARM64_REG_X19, 0x100000)
    mu.emu_start(global_fn, global_fn+0x20, count=20)
    assert mu.reg_read(UC_ARM64_REG_PC) == global_fn+0x20
    global_ratio, = struct.unpack("<f", mu.mem_read(0x100200, 4))
    return ratio, dt, global_ratio


checks = []
for hz in (30, 50, 60, 90, 100, 120):
    seconds = 1 / hz
    ratio, dt, global_ratio = execute(seconds, True)
    assert abs(ratio - seconds * 30) < 1e-6
    assert abs(dt - seconds) < 1e-7
    assert abs(global_ratio - ratio) < 1e-7
    checks.append({"hz": hz, "ratio": ratio, "seconds": dt, "global_ratio": global_ratio})
original = execute(0.01, False)
assert abs(original[0] - 0.5) < 1e-6 and abs(original[1] - 1 / 60) < 1e-6
assert abs(original[2] - 0.5) < 1e-6
print(json.dumps({"status": "pass", "version": args.version, "patched_cases": checks, "negative_control": original}, indent=2))
