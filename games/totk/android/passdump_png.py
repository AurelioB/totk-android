"""Convert a TOTK_VK_PASS_DUMP folder (p<pass>_<att>_<VkFormat>_<w>x<h>.raw) to PNG previews plus per-channel stats.
usage: python3 android/passdump_png.py <dumpdir> [outdir=<dumpdir>/png]   (needs numpy and Pillow)
Float and HDR targets are scaled by their 99th percentile; depth is stretched over its non-cleared range."""
import re, sys
from pathlib import Path
import numpy as np
from PIL import Image

def b10g11r11(u):
    def f(bits, mbits):
        e = (bits >> mbits) & 0x1F; m = bits & ((1 << mbits) - 1)
        return np.where(e == 0, m / (1 << mbits) * 2.0 ** -14, (1 + m / (1 << mbits)) * 2.0 ** (e.astype(np.int32) - 15))
    return np.stack([f(u & 0x7FF, 6), f((u >> 11) & 0x7FF, 6), f((u >> 22) & 0x3FF, 5)], -1)

def decode(fmt, b, w, h):
    """(array h x w x c as float, kind) for a VkFormat number."""
    u8 = lambda c: np.frombuffer(b, np.uint8).reshape(h, w, c) / 255.0
    if fmt in (37, 43, 41): return u8(4), 'unorm'            # R8G8B8A8 UNORM/SRGB/UINT
    if fmt in (44, 50): return u8(4)[..., [2, 1, 0, 3]], 'unorm'   # B8G8R8A8
    if fmt == 9: return u8(1), 'unorm'
    if fmt == 16: return u8(2), 'unorm'
    if fmt == 64:
        u = np.frombuffer(b, np.uint32).reshape(h, w)
        return np.stack([(u & 1023) / 1023, ((u >> 10) & 1023) / 1023, ((u >> 20) & 1023) / 1023, (u >> 30) / 3], -1), 'unorm'
    if fmt == 122: return b10g11r11(np.frombuffer(b, np.uint32).reshape(h, w).astype(np.int64)), 'float'
    if fmt in (97, 83, 76):
        c = {97: 4, 83: 2, 76: 1}[fmt]; return np.frombuffer(b, np.float16).reshape(h, w, c).astype(np.float32), 'float'
    if fmt in (100, 103, 109):
        c = {100: 1, 103: 2, 109: 4}[fmt]; return np.frombuffer(b, np.float32).reshape(h, w, c), 'float'
    if fmt == 77: return (np.frombuffer(b, np.uint16).reshape(h, w, 2) / 65535.0), 'unorm'
    if fmt == 70: return (np.frombuffer(b, np.uint16).reshape(h, w, 1) / 65535.0), 'unorm'
    if fmt == 91: return (np.frombuffer(b, np.uint16).reshape(h, w, 4) / 65535.0), 'unorm'
    if fmt == 126 or fmt == 130: return np.frombuffer(b, np.float32).reshape(h, w, 1), 'depth'
    if fmt == 124: return (np.frombuffer(b, np.uint16).reshape(h, w, 1) / 65535.0), 'depth'
    if fmt in (125, 129): return ((np.frombuffer(b, np.uint32).reshape(h, w, 1) & 0xFFFFFF) / 16777215.0), 'depth'
    return None, None

def preview(a, kind):
    a = np.nan_to_num(a.astype(np.float32), nan=0, posinf=0, neginf=0)
    if kind == 'depth':
        d = a[..., 0]; live = d[(d > 0) & (d < 1)]
        lo, hi = (np.percentile(live, 1), np.percentile(live, 99)) if live.size else (0, 1)
        g = np.clip((d - lo) / max(hi - lo, 1e-9), 0, 1)
        return np.stack([g, g, g], -1)
    if kind == 'float':
        s = np.percentile(np.abs(a[..., :3]), 99) if a.size else 1; a = np.clip(a / max(s, 1e-9), 0, 1)
    c = a.shape[-1]
    if c == 1: return np.repeat(a, 3, -1)
    if c == 2: return np.concatenate([a, np.zeros(a.shape[:2] + (1,))], -1)
    return a[..., :3]

def main():
    src = Path(sys.argv[1]); out = Path(sys.argv[2]) if len(sys.argv) > 2 else src / 'png'; out.mkdir(parents=True, exist_ok=True)
    for f in sorted(src.glob('*.raw')):
        m = re.match(r'p(\d+)_(\w+?)_(\d+)_(\d+)x(\d+)\.raw', f.name)
        if not m: continue
        fmt, w, h = int(m[3]), int(m[4]), int(m[5])
        a, kind = decode(fmt, f.read_bytes(), w, h)
        if a is None: print(f'{f.name}: format {fmt} not decoded'); continue
        Image.fromarray((preview(a, kind) * 255).astype(np.uint8)).save(out / (f.stem + '.png'))
        stats = ' '.join(f'{a[..., i].min():.3g}/{a[..., i].mean():.3g}/{a[..., i].max():.3g}' for i in range(a.shape[-1]))
        print(f'{f.stem}: {kind} min/mean/max {stats}')

main()
