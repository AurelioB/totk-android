# 通用 NSO 改写器：Switch aarch64 模块 → 可在苹果系统原生执行的映像。
# 用法：patch_nso.py <NSO 文件> <输出目录> <模块名>
# 改写（苹果内核会改 x18 与 tpidr_el0；Horizon 的线程区、svc、计时器在苹果上没有对应物）：
#   x18 读写            → 线程槽 K18（TSD，经 tpidrro_el0）
#   mrs tpidr_el0       → 读线程槽 KTLS；msr tpidr_el0 → 写 KTLS
#   mrs tpidrro_el0     → 读线程槽 KTLR（Horizon ThreadLocalRegion 指针）
#   svc #n、mrs cntpct_el0 → 宿主门（门地址取自线程槽 KGATE，进 C 后按返回点查 gates.tsv）
#   已登记剪口（CUTS，按 NSO 构建号限定）：改写前逐条断言原字节，不符即中止；结果列在 <名>.cuts.tsv
#   已登记观测门（OBS，按构建号限定）：入口一条指令换成「进宿主观测回调 → 执行原指令 → 跳回」，诊断零运行时改代码；列在 <名>.obs.tsv
# 键页只在数据段末尾保存“实际键号 × 8”，补丁代码通过键页间接取线程槽：装载器可使用任意键号。
# 输出：<名>.patched.bin（键页与补丁区接在映像后 PATCH_OFF）、<名>.fixups.tsv（布局与键页下标表头）、<名>.gates.tsv、<名>.sites.tsv、<名>.cuts.tsv、<名>.obs.tsv
# Generic NSO rewriter: Switch aarch64 module → an image that can execute natively on Apple systems.
# Usage: patch_nso.py <NSO file> <output dir> <module name>
# Rewrites (the Apple kernel modifies x18 and tpidr_el0; Horizon's thread area, svc and timers have no counterpart on Apple):
#   x18 read/write      → thread slot K18 (TSD, via tpidrro_el0)
#   mrs tpidr_el0       → read thread slot KTLS; msr tpidr_el0 → write KTLS
#   mrs tpidrro_el0     → read thread slot KTLR (Horizon ThreadLocalRegion pointer)
#   svc #n, mrs cntpct_el0 → host gate (gate address taken from thread slot KGATE; once in C, gates.tsv is looked up by return point)
#   Registered cuts (CUTS, restricted by NSO build ID): before rewriting, the original bytes are asserted one by one and any mismatch aborts; results are listed in <name>.cuts.tsv
#   Registered observation gates (OBS, restricted by build ID): one instruction at the entry is replaced with "enter the host observation callback → execute the original instruction → jump back", so diagnostics modify zero code at runtime; listed in <name>.obs.tsv
# The key page only stores "actual key number × 8" at the end of the data segment, and patch code fetches thread slots indirectly through the key page: the loader may use any key number.
# Output: <name>.patched.bin (key page and patch area appended after the image at PATCH_OFF), <name>.fixups.tsv (layout and key page index header), <name>.gates.tsv, <name>.sites.tsv, <name>.cuts.tsv, <name>.obs.tsv
import struct, re, sys, os, hashlib
from itertools import combinations
import lz4.block
from capstone import *
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM, ARM64_OP_REG   # spin waits (below)
from call_window import BLR_X18, BLR_X30, prepare_call
from relr import decode_relr
from hostreg import host_tp_words, HOST_TP_LEN

src, outdir, NAME = sys.argv[1], sys.argv[2], sys.argv[3]
d = open(src, 'rb').read()
assert d[:4] == b'NSO0'
flags = struct.unpack_from('<I', d, 0xc)[0]
segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]
bss = struct.unpack_from('<I', d, 0x3c)[0]
csz = struct.unpack_from('<III', d, 0x60)
IMG = bytearray(segs[2][1] + segs[2][2] + bss)
for i, (fo, mo, sz) in enumerate(segs):
    raw = d[fo:fo + csz[i]]
    if flags >> i & 1: raw = lz4.block.decompress(raw, uncompressed_size=sz)
    IMG[mo:mo + sz] = raw
IMG0 = bytes(IMG)   # the image as decoded, before any rewrite (spin waits keep off every word that changes)
TEXT_END = segs[0][2]
RO_OFF, DATA_OFF = segs[1][1], segs[2][1]
KEY_PAGE_OFF = (len(IMG) + 0x3FFF) & ~0x3FFF
IMG.extend(bytes(0x4000))
PATCH_OFF = (len(IMG) + 0xFFFF) & ~0xFFFF

# ---- 已登记剪口：{NSO 构建号: [(偏移, 原字节, 新字节, 理由[, 作用域])]}；上游为其他游戏构建登记的条目不随本工程分发 ----
# ---- Registered cuts: {NSO build ID: [(offset, original bytes, new bytes, reason[, scope])]}. The upstream entries (registered
# for Diablo II: Resurrected builds) are not carried: Tears of the Kingdom registers no cuts ----
CUTS = {}
# ---- Registered observation gates: {NSO build ID: [(offset, number, original instruction word, reason)]}. Numbers match OBS_* in the host's hle.h;
# the original instruction must be position independent (no PC-relative / branch / anything involving x18 or tpidr), asserted offline; when the host has no callback attached the gate block goes in and out once and returns.
# None are registered for Tears of the Kingdom ----
OBS = {}
BUILD_ID = d[0x40:0x50].hex()
# Build-time product rules: only the native rule domain is carried (the rewrite changes no guest behaviour beyond the
# registered tables, which are empty for Tears of the Kingdom).
RULESET = os.environ.get('DM_RULESET', 'native')
assert RULESET == 'native', f'unknown rule domain {RULESET}'
cuts = list(CUTS.get(BUILD_ID, []))
cuts = [c if len(c) == 5 else (*c, '全') for c in cuts]          # 第 5 项作用域：全 | 仅宿主（仅宿主 = 真值模拟器不应用） | 5th item is the scope: 全 (all) | 仅宿主 (host only) (仅宿主 = not applied by the ground-truth emulator)
for off, ob, nb, why, scope in cuts:
    ob, nb = bytes.fromhex(ob), bytes.fromhex(nb)
    assert len(ob) == len(nb) and IMG[off:off + len(ob)] == ob, f'剪口原字节不符 {off:#x}：{bytes(IMG[off:off + len(ob)]).hex()}'
    IMG[off:off + len(nb)] = nb
# 键页下标固定；键页内容由装载器写入“实际键号 × 8”。
# Key page indices are fixed; the key page content is written by the loader as "actual key number × 8".
KEYS = {'K18': 508, 'KTLS': 509, 'KTLR': 510, 'KGATE': 511, 'KRES0': 506, 'KRES1': 507, 'KASPECT': 505, 'KASPECTUI': 504}   # KRES*：分辨率参数位；KASPECT：画面宽高比上限（见 PARAMS） | KRES*: resolution parameter slots; KASPECT: picture aspect ratio cap (see PARAMS)
KEY_INDEX = {k: i for i, k in enumerate(('K18', 'KTLS', 'KTLR', 'KGATE', 'KRES0', 'KRES1', 'KASPECT', 'KASPECTUI'))}

md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
patch = bytearray(); fix = []; gates = []; stats = {}
def stat(k): stats[k] = stats.get(k, 0) + 1
def here(): return PATCH_OFF + len(patch)
def emit(w, kind=None):
    if kind: fix.append((len(patch), kind))
    patch.extend(struct.pack('<I', w & 0xFFFFFFFF))

def emit_host_tp(reg):
    """x<reg> = the host thread's slot-block base (Darwin: mrs tpidrro_el0; Linux / Android: mrs tpidr_el0 + ldr [.., #16], hostreg.py)."""
    for w in host_tp_words(reg): emit(w)

def emit_key_addr(reg, kind):
    fix.append((len(patch), kind)); emit(ADRP(reg, here(), KEY_PAGE_OFF)); emit(LDRX(reg, reg, KEY_INDEX[kind]))

def emit_slot_addr(base, key_reg, kind):
    emit_key_addr(key_reg, kind); emit(ADDR(base, base, key_reg))

# ---- 编码器 ----
# ---- Encoders ----
def B(frm, to):
    off = to - frm; assert -(1 << 27) <= off < (1 << 27) and off % 4 == 0
    return 0x14000000 | ((off >> 2) & 0x3FFFFFF)
def STP(t, t2, n, off): return 0xA9000000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def LDP(t, t2, n, off): return 0xA9400000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def STP_PRE(t, t2, n, off): return 0xA9800000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def LDP_POST(t, t2, n, off): return 0xA8C00000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def STUR(t, n, off): return 0xF8000000 | ((off & 0x1FF) << 12) | (n << 5) | t   # stur xt,[xn,#off]（off 可负） | stur xt,[xn,#off] (off may be negative)
def LDUR(t, n, off): return 0xF8400000 | ((off & 0x1FF) << 12) | (n << 5) | t   # ldur xt,[xn,#off]
def LDRX(t, n, imm12=0): return 0xF9400000 | (imm12 << 10) | (n << 5) | t
def STRX(t, n, imm12=0): return 0xF9000000 | (imm12 << 10) | (n << 5) | t
def ADRP(dd, pc, target):
    imm = ((target & ~0xFFF) - (pc & ~0xFFF)) >> 12; assert -(1 << 20) <= imm < (1 << 20)
    return 0x90000000 | ((imm & 3) << 29) | (((imm >> 2) & 0x7FFFF) << 5) | dd
def ADDI(dd, n, imm12, sh=0): return 0x91000000 | (sh << 22) | (imm12 << 10) | (n << 5) | dd
def ADDR(dd, n, m): return 0x8B000000 | (m << 16) | (n << 5) | dd
def SUBI(dd, n, imm12, sh=0): return 0xD1000000 | (sh << 22) | (imm12 << 10) | (n << 5) | dd
def BR(n): return 0xD61F0000 | (n << 5)
def BLR(n): return 0xD63F0000 | (n << 5)
NOP = 0xD503201F

def dis(w, addr):
    l = list(md.disasm(struct.pack('<I', w), addr)); assert len(l) == 1, hex(w)
    return l[0]
def regset(ins):
    r, w = ins.regs_access()
    return {ins.reg_name(x) for x in r}, {ins.reg_name(x) for x in w}
def num(name):
    m = re.fullmatch(r'[xw](\d+)', name or ''); return int(m.group(1)) if m else None
def has18(ins): return re.search(r'\b[xw]18\b', ins.op_str) is not None
def regs_in(ins):
    s = {int(n) for n in re.findall(r'\b[xw](\d+)\b', ins.op_str)}
    try:
        rd, wr = regset(ins); s |= {num(n) for n in rd | wr} - {None}
    except Exception: pass
    return s
def pick(avoid, n=2):
    out = [r for r in range(0, 16) if r not in avoid and r != 18][:n]; assert len(out) == n; return out

def subst(ins, S):
    """x18/w18 → xS/wS：只改 4 个寄存器字段位置，用反汇编文本核对。 | x18/w18 → xS/wS: only the 4 register field positions are changed, verified against the disassembly text."""
    w = struct.unpack('<I', ins.bytes)[0]
    want = re.sub(r'\b([xw])18\b', lambda m: f'{m.group(1)}{S}', ins.op_str)
    pos = [p for p in (0, 5, 10, 16) if (w >> p) & 31 == 18]
    for k in range(len(pos), 0, -1):
        for c in combinations(pos, k):
            nw = w
            for p in c: nw = (nw & ~(31 << p)) | (S << p)
            try: dd = dis(nw, ins.address)
            except AssertionError: continue
            if dd.mnemonic == ins.mnemonic and dd.op_str == want: return nw
    raise RuntimeError(f'subst fail {ins.address:x} {ins.mnemonic} {ins.op_str}')

# ---- 各类改写块 ----
# ---- Rewrite blocks of each kind ----
def sp_delta(ins):
    """指令对 sp 的写回量（前变址 [sp, #i]! / 后变址 [sp], #i）；不写回 sp 返回 0。 | The amount an instruction writes back to sp (pre-index [sp, #i]! / post-index [sp], #i); returns 0 if sp is not written back."""
    m = re.search(r'\[sp, #(-?(?:0x)?[0-9a-f]+)\]!', ins.op_str)
    if m: return int(m.group(1), 0)
    m = re.search(r'\[sp\], #(-?(?:0x)?[0-9a-f]+)$', ins.op_str)
    if m: return int(m.group(1), 0)
    assert not re.match(r'(sp|wsp),', ins.op_str), f'writes sp: {ins.address:x} {ins.op_str}'
    return 0

def site_plain(ins):
    S, T = pick(regs_in(ins))
    dlt = sp_delta(ins)
    save = -16 if dlt == 0 else -112          # 改 sp 的指令会写到 sp 下方，暂存放到保留区深处 | Instructions that modify sp write below sp, so the scratch save is placed deep in the reserved area
    assert dlt == 0 or dlt >= -96, f'sp delta {dlt} at {ins.address:x}'
    start = here()
    emit(STP(S, T, 31, save)); emit_host_tp(T); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    emit(subst(ins, S))
    emit(STRX(S, T))                 # 一律写回：capstone 会漏报写入（adds w18 实例） | Always write back: capstone can miss reporting a write (the adds w18 instance)
    emit(LDP(S, T, 31, save - dlt)); emit(B(here(), ins.address + 4))
    return start

def site_sp(ins):
    """Destination is sp and a source is x18 (an alloca: sub x18, x11, x9; mov sp, x18 - Pokemon Pokopia's main+0x1a181dc; Engage's
    mov x18, sp; sub x18, x18, x0; mov sp, x18 in seedds/nxnative, whose site_sp this is, MIT). site_plain saves its scratch relative
    to the old sp and would restore it relative to the new one: copy the old sp into T first, and after the instruction restore
    S / T relative to T (without writeback, ldp may use the base register as a destination). x18 is not written, so the slot is
    not written back."""
    S, T = pick(regs_in(ins))
    start = here()
    emit(STP(S, T, 31, -16)); emit_host_tp(T); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    emit(ADDI(T, 31, 0))             # mov T, sp (in add immediate, 31 is sp)
    emit(subst(ins, S))
    emit(LDP(S, T, T, -16)); emit(B(here(), ins.address + 4))
    return start

def site_cond(ins):
    w = struct.unpack('<I', ins.bytes)[0]
    S, T = pick(regs_in(ins))
    tgt = ins.operands[-1].imm
    start = here()
    emit(STP(S, T, 31, -16)); emit_host_tp(T); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    m = ins.mnemonic
    taken = here() + 4 * 3
    off = (taken - here()) >> 2
    nw = ((w & 0xFF000000) | ((off & 0x7FFFF) << 5) | S) if m in ('cbz', 'cbnz') else ((w & 0xFFF80000) | ((off & 0x3FFF) << 5) | S)
    pc = here(); emit(nw)
    dd = dis(nw, pc); assert dd.mnemonic == m and dd.operands[-1].imm == taken
    emit(LDP(S, T, 31, -16)); emit(B(here(), ins.address + 4))
    assert here() == taken
    emit(LDP(S, T, 31, -16)); emit(B(here(), tgt))
    return start

def site_pcrel(ins):
    tgt = ins.operands[1].imm
    S, T = pick(set())
    start = here()
    emit(STP(S, T, 31, -16)); emit_host_tp(T); emit_slot_addr(T, S, 'K18')   # S 先作键页偏移暂存，下一条再装目标值 | S first serves as a temporary for the key page offset; the next instruction then loads the target value
    pc = here(); emit(ADRP(S, pc, tgt))
    if ins.mnemonic == 'adr': emit(ADDI(S, S, tgt & 0xFFF))
    emit(STRX(S, T))
    emit(LDP(S, T, 31, -16)); emit(B(here(), ins.address + 4))
    return start

def site_br(ins, base, count):
    """跳转表 br x18：目标 = base + 4i。x16 暂存红区 → 等距落地垫 → 恢复 x16 → 原目标。 | Jump table br x18: target = base + 4i. Stash x16 in the red zone → equally spaced landing pads → restore x16 → original target."""
    start = here()
    emit(STUR(16, 31, -16)); emit(STUR(17, 31, -24))   # x16 / x17 是函数内可存活的临时寄存器，借用前存红区 | x16 / x17 are temporary registers that can stay live within a function; save them to the red zone before borrowing
    emit_host_tp(16); emit_slot_addr(16, 17, 'K18'); emit(LDRX(16, 16)); emit(LDUR(17, 31, -24))
    padbase = start + 4 * 24
    delta = padbase - base
    op = ADDI if delta >= 0 else SUBI; dl = abs(delta)
    ops = []; hi, lo = dl >> 12, dl & 0xFFF
    while hi: h = min(hi, 0xFFF); ops.append(op(16, 16, h, 1)); hi -= h
    if lo: ops.append(op(16, 16, lo, 0))
    assert len(ops) <= 12
    for o in ops: emit(o)
    emit(BR(16))
    assert here() <= padbase, f'jump-table prologue overruns its landing pads at {ins.address:x}'
    while here() < padbase: emit(NOP)
    tails = padbase + 4 * count
    for i in range(count): emit(B(here(), tails + 8 * i))
    for i in range(count):
        emit(0xF85F0000 | (31 << 5) | 16)      # ldur x16,[sp,#-16]
        emit(B(here(), base + 4 * i))
    return start

def site_sysreg_read(ins, kind):
    """mrs xN, tpidr_el0 / tpidrro_el0 → xN = 线程槽值。 | mrs xN, tpidr_el0 / tpidrro_el0 → xN = thread slot value."""
    N = ins.bytes[0] & 31
    tmp = 16 if N != 16 else 17
    start = here()
    emit(STUR(tmp, 31, -16)); emit_host_tp(N); emit_slot_addr(N, tmp, kind); emit(LDRX(N, N)); emit(LDUR(tmp, 31, -16))   # tmp 存红区借用 | tmp is saved by borrowing the red zone
    emit(B(here(), ins.address + 4))
    return start

def site_tpidr_write(ins):
    N = struct.unpack('<I', ins.bytes)[0] & 31
    (T,) = pick({N}, 1)
    start = here()
    emit(0xF81F0000 | (31 << 5) | T)           # stur xT,[sp,#-16]
    tmp = next(x for x in (16, 17) if x not in (N, T))
    emit(STUR(tmp, 31, -24)); emit_host_tp(T); emit_slot_addr(T, tmp, 'KTLS'); emit(STRX(N, T)); emit(LDUR(tmp, 31, -24))
    emit(0xF85F0000 | (31 << 5) | T)           # ldur xT,[sp,#-16]
    emit(B(here(), ins.address + 4))
    return start

def emit_gate_call():
    """门块公共段（紧跟 stp x29,x30,[sp,#-16]! 之后、blr x30 之前）：x30 = 线程槽 KGATE 的门地址。
    键页取槽要借第二个寄存器，借 x29 后立即从栈上恢复：_dm_gate 存下的 x29 必须是客体原值——
    宿主按 [x29+8] 认调用者（技能绑定观测门 6、私有瞄准门 svc86）、崩溃回溯也走 x29 链
    （r24 方案 C 起借了不还，宿主拿到的 x29 是键页里的槽偏移，技能图标退回占位字）。
    Common section of a gate block (right after stp x29,x30,[sp,#-16]! and before blr x30): x30 = the gate address in thread slot KGATE.
        Fetching the slot through the key page needs a second register to be borrowed; x29 is borrowed and then immediately restored from the stack: the x29 saved by _dm_gate must be the guest's original value --
        the host identifies the caller by [x29+8] (skill binding observation gate 6, private aiming gate svc86), and the crash backtrace also walks the x29 chain
        (since r24 plan C it was borrowed and not returned, the x29 the host got was the slot offset from the key page, and skill icons fell back to placeholder text)."""
    emit_host_tp(30); emit_slot_addr(30, 29, 'KGATE'); emit(LDRX(30, 30)); emit(LDRX(29, 31))

def site_gate(ins, kind, arg):
    """svc / mrs cntpct_el0 → 宿主门。门地址取自线程槽 KGATE；门按返回点（本块 ldp 的地址）查 gates.tsv。 | svc / mrs cntpct_el0 → host gate. The gate address is taken from thread slot KGATE; the gate looks up gates.tsv by return point (the address of this block's ldp)."""
    start = here()
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call()
    emit(BLR(30))
    ret = here()
    emit(LDP_POST(29, 30, 31, 16))
    emit(B(here(), ins.address + 4))
    gates.append((ret, kind, arg, ins.address))
    return start

def site_obs(off, oid, word):
    """观测门：入口原指令 → 进宿主门（arg = 观测编号）→ 恢复 x29/x30 → 执行原指令 → 跳回 off+4。
    门块前半与 site_gate 逐字同构；宿主只读寄存器、不改（dm_gate 回写的 x0–x8 即原值）。
    Observation gate: original entry instruction → enter the host gate (arg = observation number) → restore x29/x30 → execute the original instruction → jump back to off+4.
        The first half of the gate block is word-for-word isomorphic with site_gate; the host only reads registers and does not modify them (the x0–x8 written back by dm_gate are the original values)."""
    ins = dis(word, off)
    assert ins.mnemonic not in ('adr', 'adrp', 'b', 'bl', 'br', 'blr', 'ret', 'cbz', 'cbnz', 'tbz', 'tbnz') and not ins.mnemonic.startswith('b.') \
        and not (ins.mnemonic.startswith('ldr') and '[' not in ins.op_str) and not has18(ins), f'观测门原指令不可搬 {off:#x} {ins.mnemonic} {ins.op_str}'
    start = here()
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call()
    emit(BLR(30))
    ret = here()
    emit(LDP_POST(29, 30, 31, 16))
    emit(word)
    emit(B(here(), off + 4))
    gates.append((ret, 'obs', oid, off))
    return start

# ---- 独占窗口 ----
# ---- Exclusive windows ----
EXCL_LD = ('ldxr', 'ldaxr', 'ldxp', 'ldaxp', 'ldxrb', 'ldaxrb', 'ldxrh', 'ldaxrh')
EXCL_ST = ('stxr', 'stlxr', 'stxp', 'stlxp', 'stxrb', 'stlxrb', 'stxrh', 'stlxrh')
def find_windows(insl):
    out = []
    for i, ins in enumerate(insl):
        if ins.mnemonic not in EXCL_LD: continue
        j = i + 1
        while j < len(insl) and insl[j].mnemonic not in EXCL_ST + EXCL_LD: j += 1
        if j >= len(insl) or j - i >= 16 or insl[j].mnemonic not in EXCL_ST: continue
        if j + 1 < len(insl) and insl[j + 1].mnemonic in ('cbz', 'cbnz'): j += 1
        seg = insl[i:j + 1]
        if any(has18(x) for x in seg): out.append(seg)
    return out

def linked_windows(insl, windows):
    """Move an out-of-line LDXR retry together with the STXR it re-enters.

    The TSD/scratch prologue must precede LDXR. Entering a suffix trampoline
    between LDXR and STXR would insert stores into the reservation interval
    and can prevent the retry from making progress on real hardware.
    """
    groups = {}
    edges = set()
    original = list(windows)
    for j, ins in enumerate(insl):
        if not (ins.mnemonic in ('cbz', 'cbnz', 'tbz', 'tbnz') or ins.mnemonic.startswith('b.')):
            continue
        target = ins.operands[-1].imm
        owner = next((seg for seg in original if seg[0].address < target <= seg[-1].address), None)
        if owner is None or owner[0].address <= ins.address <= owner[-1].address:
            continue
        k = j
        while k >= max(0, j - 15) and insl[k].mnemonic not in EXCL_LD:
            k -= 1
        assert k >= max(0, j - 15), f'exclusive alternate entry lacks LDXR: {ins.address:x}'
        retry = insl[k:j + 1]
        assert all(x.mnemonic not in EXCL_ST and not x.mnemonic.startswith(('st', 'bl'))
                   and x.mnemonic not in ('b', 'br', 'ret')
                   and not x.mnemonic.startswith('b.') for x in retry[:-1]), f'unsafe exclusive retry: {ins.address:x}'
        assert all(not any(seg[0].address <= x.address <= seg[-1].address for seg in original)
                   for x in retry), f'overlapping exclusive retry: {ins.address:x}'
        group = groups.setdefault(owner[0].address, [owner])
        if not any(seg[0].address == retry[0].address for seg in group):
            group.append(retry); windows.append(retry)
        edges.add((ins.address, target))
    return list(groups.values()), edges

def site_window_group(segments):
    used = set()
    for seg in segments:
        for ins in seg: used |= regs_in(ins)
    S, T = pick(used)
    entries = {}; entry_branches = []
    for seg in segments:
        entries[seg[0].address] = here()
        emit(STP(S, T, 31, -16)); emit_host_tp(T); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
        entry_branches.append((len(patch), seg[0].address)); emit(NOP)
    copied = {}; cursor = here()
    for seg in segments:
        for ins in seg: copied[ins.address] = cursor; cursor += 4
        cursor += 4  # explicit fallthrough from each fragment
    exits = []
    for seg in segments:
        exits.append(seg[-1].address + 4)
        for ins in seg:
            if ins.mnemonic in ('b', 'cbz', 'cbnz', 'tbz', 'tbnz') or ins.mnemonic.startswith('b.'):
                target = ins.operands[-1].imm
                if target not in copied and target not in exits: exits.append(target)
    exits = list(dict.fromkeys(exits))
    exit_addresses = {target: cursor + 16 * i for i, target in enumerate(exits)}
    for offset, target in entry_branches:
        struct.pack_into('<I', patch, offset, B(PATCH_OFF + offset, copied[target]))
    for seg in segments:
        for ins in seg:
            assert here() == copied[ins.address]
            word = subst(ins, S) if has18(ins) else struct.unpack('<I', ins.bytes)[0]
            mnemonic = ins.mnemonic
            if mnemonic in ('b', 'cbz', 'cbnz', 'tbz', 'tbnz') or mnemonic.startswith('b.'):
                target = ins.operands[-1].imm
                destination = copied.get(target, exit_addresses.get(target))
                delta = (destination - here()) >> 2
                if mnemonic == 'b': word = B(here(), destination)
                elif mnemonic in ('tbz', 'tbnz'): word = (word & 0xFFF8001F) | ((delta & 0x3FFF) << 5)
                else: word = (word & 0xFF00001F) | ((delta & 0x7FFFF) << 5)
                assert dis(word, here()).operands[-1].imm == destination
            else:
                assert mnemonic not in ('adr', 'adrp', 'bl', 'br', 'blr', 'ret') and not (mnemonic == 'ldr' and '[' not in ins.op_str)
            emit(word)
        emit(B(here(), copied.get(seg[-1].address + 4, exit_addresses[seg[-1].address + 4])))
    for target in exits:
        assert here() == exit_addresses[target]
        emit(STRX(S, T)); emit(LDP(S, T, 31, -16)); emit(B(here(), target)); emit(NOP)
    return entries

def site_window(seg):
    used = set()
    for x in seg: used |= regs_in(x)
    S, T = pick(used)
    lo, hi = seg[0].address, seg[-1].address + 4
    start = here()
    emit(STP(S, T, 31, -16)); emit_host_tp(T); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    copy0 = here(); n = len(seg)
    exits = copy0 + 4 * n
    # The copied body falls through to the first exit stub. That stub MUST be
    # hi; placing an external conditional target first misroutes the untaken
    # edge (TOTK main+0x72afdc and main+0x7aa630).
    ex_targets = [hi]; words = []
    for k, x in enumerate(seg):
        w = struct.unpack('<I', x.bytes)[0]
        if has18(x): w = subst(x, S)
        m = x.mnemonic
        if m in ('cbz', 'cbnz', 'tbz', 'tbnz') or m.startswith('b.'):
            tgt = x.operands[-1].imm
            if lo <= tgt < hi: newt = copy0 + (tgt - lo)
            else: ex_targets.append(tgt); newt = exits + 16 * (len(ex_targets) - 1)
            pc = copy0 + 4 * k; off = (newt - pc) >> 2
            if m in ('tbz', 'tbnz'): w = (w & 0xFFF8001F) | ((off & 0x3FFF) << 5)
            else: w = (w & 0xFF00001F) | ((off & 0x7FFFF) << 5)
            dd = dis(w, pc); assert dd.operands[-1].imm == newt and dd.mnemonic == m
        else:
            assert m not in ('adr', 'adrp', 'b', 'bl', 'br', 'blr', 'ret') and not (m == 'ldr' and '[' not in x.op_str), hex(x.address)
        words.append(w)
    for w in words: emit(w)
    for t in ex_targets:
        emit(STRX(S, T)); emit(LDP(S, T, 31, -16)); emit(B(here(), t)); emit(NOP)
    return start, lo, hi

# ---- 函数边界（eh_frame_hdr）----
# ---- Function boundaries (eh_frame_hdr) ----
m0 = struct.unpack_from('<I', IMG, 4)[0]
assert IMG[m0:m0 + 4] == b'MOD0'
ehs = m0 + struct.unpack_from('<i', IMG, m0 + 16)[0]
def rd(o, enc):
    fmt, app = enc & 0xF, enc & 0x70
    v, sz = {0x3: (struct.unpack_from('<I', IMG, o)[0], 4), 0xB: (struct.unpack_from('<i', IMG, o)[0], 4),
             0x4: (struct.unpack_from('<Q', IMG, o)[0], 8)}[fmt]
    if app == 0x10: v += o
    if app == 0x30: v += ehs
    return v, sz
_, penc, cenc, tenc = IMG[ehs:ehs + 4]
o = ehs + 4
_, s = rd(o, penc); o += s
nfde, s = rd(o, cenc); o += s
starts = set()
for _ in range(nfde):
    a, s = rd(o, tenc); o += s
    _, s = rd(o, tenc); o += s
    starts.add(a)
starts = sorted(x for x in starts if x < TEXT_END)

# ---- 线性扫描找全部改写点（不靠寄存器读写报告）----
# ---- Linear scan to find all rewrite points (not relying on register read/write reports) ----
kinds = {}
for o in range(0, TEXT_END, 4):
    w = struct.unpack_from('<I', IMG, o)[0]
    if w & 0xFFE0001F == 0xD4000001: kinds[o] = ('svc', (w >> 5) & 0xFFFF); continue
    if w & 0xFFFFFFE0 == 0xD53BD040: kinds[o] = ('tpidr_r', None); continue
    if w & 0xFFFFFFE0 == 0xD51BD040: kinds[o] = ('tpidr_w', None); continue
    if w & 0xFFFFFFE0 == 0xD53BD060: kinds[o] = ('tpidrro_r', None); continue
    if w & 0xFFFFFFE0 == 0xD53BE020: kinds[o] = ('cntpct', w & 31); continue
    if w & 0xFFFFFFE0 == 0xD53BE040: kinds[o] = ('cntvct', w & 31); continue
    for ins in md.disasm(IMG[o:o + 4], o):
        if has18(ins): kinds[o] = ('x18', None)

# 按函数取指令表，找窗口与跳转表
# Take the instruction table per function, find the windows and jump tables
import bisect
def func_of(a):
    i = bisect.bisect_right(starts, a) - 1
    return (starts[i], starts[i + 1] if i + 1 < len(starts) else TEXT_END) if i >= 0 else (a, a + 4)
done = set(); windows = []; window_groups = []; jumps = {}; calls = {}
# Signed 32-bit offset tables added in TOTK 1.4.3. Validate both the dispatch
# instructions and every table entry before using a bounded landing-pad span.
# Each tuple is (ADR base, table address, entry count, SHA-256 of the 20 bytes ending at the br, SHA-256 of the entries);
# no game bytes are stored here, the entries are read from the image once their hash matches.
SIGNED_TABLES = {'277178b7dba1b6d4949a84778d4abc58': {
    0x393bb0: (0x393ba0, 0x2d8626c, 6, 'c4b04a76294666a64d2781ce7baeace80a5f4c7d642e8fa3ce378f654f5a2db0', 'd33595fbce2691ca1f39bbc1c6844276e2b186fca36257fd37a47eb5300de0ce'),
    0x596d30: (0x596d20, 0x2d89154, 9, '4013676edf743295ec1ebf998686b48611e56940a0c1dc3ae3b4181ca4a82301', 'b631a17de2c73343aaee6d69f1b8a9d405da78dc04d47340e1bff9ca5a8bdafe'),
    0x598ef4: (0x598ee4, 0x2d89038, 9, '9870cc3d2f0c71d6a09d96e7ac43fcf70d76695ff463760694559035fb4967f1', '35d321f36b6efb7fd9781f31fe86ec09fd86e4be05102e5316e73fb6a5fe3ab2'),
    0x5f2a9c: (0x5f2a90, 0x2d892e0, 5, 'd67af8cdfbb0358495b476292eae9c7b52067e0d38ab4439a3ba760eb21f0f4a', 'c3511ac589afaca4b75f001ad082a90aef06118c7e65d7f9076ab78dc862f0f2'),
    0x6397d8: (0x6397cc, 0x2d88e7c, 7, 'f3bc8a30d4d2885e8c328e9c2d145f9a24ad89f36e72ca52bd179f1733016f46', '419fdee7c1384717b79e08e702eb2ca83db9aa68468e28468915e2c03230e98e'),
}}
FLAG_SETTERS = ('cmp', 'cmn', 'tst', 'adds', 'subs', 'ands', 'bics', 'adcs', 'sbcs', 'ccmp', 'ccmn', 'fcmp', 'fcmpe', 'fccmp', 'fccmpe', 'negs', 'ngcs')
def ldrsw_table(insl, j, fa, fb):
    """Breath of the Wild 1.6.0 (older clang): a table of signed 32-bit offsets from its own base,
        cmp wK, #n-1 ; b.hi default ; [mov wI, wK] ; adrp xT, page ; add xT, xT, #lo ; ldrsw xE, [xT, xI, lsl #2] ; add x18, xE, xT ; br x18
    (T, E, I may be x18). The table is read from the image; returns (base, count) covering every target, each one checked to be an
    instruction of this function, or None when the dispatch does not have exactly this shape."""
    ops = lambda p: [o.strip() for o in p.op_str.replace('[', '').replace(']', '').split(',')]
    ctx = insl[max(0, j - 48):j]
    writes = lambda p, r: r is not None and p.mnemonic not in ('str', 'stur', 'stp', 'strb', 'strh', 'cmp', 'cmn', 'tst', 'b', 'bl', 'br', 'blr', 'ret',
                                                               'cbz', 'cbnz', 'tbz', 'tbnz') \
        and not p.mnemonic.startswith('b.') and ops(p)[0] in (f'x{r}', f'w{r}')
    k = len(ctx) - 1
    if k < 0 or ctx[k].mnemonic != 'add' or len(ops(ctx[k])) != 3 or ops(ctx[k])[0] != 'x18':
        return None
    a, b = ops(ctx[k])[1:]
    for k2 in range(k - 1, -1, -1):              # the load of the entry: ldrsw xE, [xT, xI, lsl #2] with {E, T} = {a, b}
        p = ctx[k2]
        if p.mnemonic == 'ldrsw' and 'lsl #2' in p.op_str:
            o = ops(p)
            if len(o) == 4 and {o[0], o[1]} == {a, b} and o[0] != o[1]:
                break
        if writes(p, num(a)) or writes(p, num(b)):
            return None
    else:
        return None
    E, T, I = num(o[0]), num(o[1]), num(o[2])
    if None in (E, T, I) or any(writes(p, T) or writes(p, I) for p in ctx[k2 + 1:k]):
        return None
    k3 = k2 - 1                                  # the table base: adrp xT, page ; add xT, xT, #lo
    while k3 >= 0 and not writes(ctx[k3], T): k3 -= 1
    if k3 < 1 or ctx[k3].mnemonic != 'add' or ops(ctx[k3])[:2] != [f'x{T}', f'x{T}'] or not ops(ctx[k3])[2].startswith('#'):
        return None
    lo = int(ops(ctx[k3])[2][1:], 0)
    k4 = k3 - 1
    while k4 >= 0 and not writes(ctx[k4], T): k4 -= 1
    if k4 < 0 or ctx[k4].mnemonic != 'adrp' or ops(ctx[k4])[0] != f'x{T}':
        return None
    table = ctx[k4].operands[1].imm + lo
    src = {I}; k5 = k2 - 1                      # the bound: b.hi / b.hs after a cmp on the index or on the register it was copied from
    while k5 >= 0 and ctx[k5].mnemonic not in ('b.hi', 'b.hs'):
        p = ctx[k5]
        if p.mnemonic in FLAG_SETTERS or p.mnemonic in ('b', 'br', 'ret') or p.mnemonic.startswith('b.'):
            return None
        if p.mnemonic == 'mov' and len(ops(p)) == 2 and num(ops(p)[0]) in src and num(ops(p)[1]) is not None:
            src = {num(ops(p)[1])}
        elif any(writes(p, r) for r in src):
            return None
        k5 -= 1
    if k5 < 1:
        return None
    k6 = k5 - 1
    while k6 >= 0 and ctx[k6].mnemonic not in FLAG_SETTERS:
        if any(writes(ctx[k6], r) for r in src) or ctx[k6].mnemonic.startswith('b'):
            return None
        k6 -= 1
    if k6 < 0 or ctx[k6].mnemonic != 'cmp' or len(ops(ctx[k6])) != 2 or num(ops(ctx[k6])[0]) not in src or not ops(ctx[k6])[1].startswith('#'):
        return None
    n = int(ops(ctx[k6])[1][1:], 0) + (1 if ctx[k5].mnemonic == 'b.hi' else 0)
    if not 0 < n <= 4096:
        return None
    entries = struct.unpack_from('<' + 'i' * n, IMG, table)
    targets = [table + e for e in entries]
    assert all(fa <= t < fb and t % 4 == 0 for t in targets), f'ldrsw table {insl[j].address:x}: target outside its function'
    return min(targets), (max(targets) - min(targets)) // 4 + 1

funcs = sorted({func_of(a) for a, (k, _) in kinds.items() if k == 'x18'})
for fa, fb in funcs:
    insl = list(md.disasm(bytes(IMG[fa:fb]), fa))
    ws = find_windows(insl)
    groups, linked_edges = linked_windows(insl, ws)
    window_groups.extend(groups)
    for wseg in ws:
        windows.append(wseg); done |= {x.address for x in wseg}
    for j, ins in enumerate(insl):
        if ins.mnemonic == 'br' and ins.op_str == 'x18':
            base = width = None
            ctx = insl[max(0, j - 32):j]  # TOTK: arithmetic may follow the table-address setup.
            for p in reversed(ctx):
                if p.mnemonic == 'adr' and p.op_str.startswith('x18,'): base = p.operands[1].imm; break
            for p in reversed(ctx):
                if p.mnemonic == 'add' and p.op_str.startswith('x18, x18,') and 'lsl #2' in p.op_str:
                    idx = num(p.reg_name(p.operands[2].reg))
                    for q in reversed(ctx):
                        if q.mnemonic in ('ldrb', 'ldrsb') and num(q.reg_name(q.operands[0].reg)) == idx: width = 256
                        if q.mnemonic in ('ldrh', 'ldrsh') and num(q.reg_name(q.operands[0].reg)) == idx: width = 65536
                        if width: break
                    break
            if not width and ins.address in SIGNED_TABLES.get(BUILD_ID, {}):
                adrbase, table, n, dispatch_sha, table_sha = SIGNED_TABLES[BUILD_ID][ins.address]
                assert hashlib.sha256(IMG[ins.address - 16:ins.address + 4]).hexdigest() == dispatch_sha, f'signed-table dispatch {ins.address:x}'
                assert hashlib.sha256(IMG[table:table + 4 * n]).hexdigest() == table_sha, f'signed-table entries {ins.address:x}'
                targets = [adrbase + offset for offset in struct.unpack_from('<' + 'i' * n, IMG, table)]
                assert all(0 <= target < TEXT_END and target % 4 == 0 for target in targets)
                base, width = min(targets), (max(targets) - min(targets)) // 4 + 1
            if not width:
                found = ldrsw_table(insl, j, fa, fb)
                if found:
                    base, width = found; stat('ldrsw_table')
            assert base and width, f'jump table {ins.address:x}'
            jumps[ins.address] = (base, width)
        if ins.mnemonic == 'blr' and ins.op_str == 'x18':
            assert j > 0, f'call without prefix {ins.address:x}'
            previous = insl[j - 1]
            assert previous.address + 4 == ins.address
            calls[previous.address] = (ins.address, struct.unpack('<I', previous.bytes)[0])
    # 不许有跳转落进窗口中间
    # No jump may land in the middle of a window
    inwin = {x.address for wseg in ws for x in wseg}; heads = {wseg[0].address for wseg in ws}
    for x in insl:
        if x.mnemonic in ('b', 'bl', 'cbz', 'cbnz', 'tbz', 'tbnz') or x.mnemonic.startswith('b.'):
            t = x.operands[-1].imm
            assert not (t in inwin and t not in heads and (x.address, t) not in linked_edges), f'branch into window {x.address:x}'

# A call must always execute its preparation word; reject known alternate entry.
if calls:
    call_targets = {a for a, _ in calls.values()}
    assert not call_targets & set(starts), 'function entry into call-window second word'
    for address in range(0, TEXT_END, 4):
        word = struct.unpack_from('<I', IMG, address)[0]
        target = None
        if word & 0x7C000000 == 0x14000000:
            target = address + 4 * (((word & 0x3FFFFFF) ^ 0x2000000) - 0x2000000)
        elif word & 0xFF000010 == 0x54000000 or word & 0x7E000000 == 0x34000000:
            target = address + 4 * ((((word >> 5) & 0x7FFFF) ^ 0x40000) - 0x40000)
        elif word & 0x7E000000 == 0x36000000:
            target = address + 4 * ((((word >> 5) & 0x3FFF) ^ 0x2000) - 0x2000)
        assert target not in call_targets, f'branch into call without preparation {address:x}'
    for prefix, (call, word) in calls.items():
        assert prefix not in kinds and prefix not in done and call not in done, f'overlapping call window {call:x}'
    # Check dynamic definitions and RELA pointer targets as well as direct branches.
    dyn = m0 + struct.unpack_from('<i', IMG, m0 + 4)[0]
    tags = {}
    while True:
        tag, value = struct.unpack_from('<qQ', IMG, dyn); dyn += 16
        if tag == 0: break
        tags[tag] = value
    count = struct.unpack_from('<I', IMG, tags[4] + 4)[0]
    for where, addend in decode_relr(IMG, tags, DATA_OFF, KEY_PAGE_OFF):
        assert addend not in call_targets, f'RELR pointer enters unprepared call: {where:x}'
    for i in range(count):
        _, _, _, section, value, _ = struct.unpack_from('<IBBHQQ', IMG, tags[6] + 24 * i)
        assert not section or value not in call_targets, 'dynamic symbol enters unprepared call'
    for table, size in ((tags.get(7), tags.get(8, 0)), (tags.get(23), tags.get(2, 0))):
        if table:
            for off in range(table, table + size, 24):
                _, info, addend = struct.unpack_from('<QQq', IMG, off)
                assert info & 0xFFFFFFFF != 1027 or addend not in call_targets, 'relative pointer enters unprepared call'

def put_branch(at, to): struct.pack_into('<I', IMG, at, B(at, to))
grouped_heads = set()
for group in window_groups:
    for head, entry in site_window_group(group).items():
        grouped_heads.add(head); put_branch(head, entry); stat('window')
for wseg in windows:
    if wseg[0].address in grouped_heads: continue
    st, lo, hi = site_window(wseg); stat('window'); put_branch(lo, st)
for prefix, (call, word) in sorted(calls.items()):
    st = here()
    block = prepare_call(word, prefix, call, st, KEY_PAGE_OFF, KEY_INDEX['K18'])
    fix.append((len(patch) + 4 * (2 + HOST_TP_LEN), 'K18'))   # the window's adrp of the key page
    patch.extend(block)
    put_branch(prefix, st)
    assert struct.unpack_from('<I', IMG, call)[0] == BLR_X18
    struct.pack_into('<I', IMG, call, BLR_X30)
    done.update((prefix, call)); stat('call_window')
for a in sorted(kinds):
    if a in done: continue
    k, arg = kinds[a]
    ins = dis(struct.unpack_from('<I', IMG, a)[0], a)
    if k == 'x18':
        m = ins.mnemonic
        if m == 'br': st = site_br(ins, *jumps[a]); stat('br')
        elif m in ('cbz', 'cbnz', 'tbz', 'tbnz'): st = site_cond(ins); stat('cond')
        elif m in ('adr', 'adrp'): st = site_pcrel(ins); stat('pcrel')
        elif m in ('blr', 'ret', 'b', 'bl') or (m == 'ldr' and '[' not in ins.op_str): raise RuntimeError(f'{a:x} {m} {ins.op_str}')
        elif re.match(r'(sp|wsp),', ins.op_str): st = site_sp(ins); stat('sp')
        else: st = site_plain(ins); stat('plain')
    elif k == 'tpidr_r': st = site_sysreg_read(ins, 'KTLS'); stat(k)
    elif k == 'tpidrro_r': st = site_sysreg_read(ins, 'KTLR'); stat(k)
    elif k == 'tpidr_w': st = site_tpidr_write(ins); stat(k)
    elif k == 'svc': st = site_gate(ins, 'svc', arg); stat(k)
    elif k in ('cntpct', 'cntvct'): st = site_gate(ins, k, arg); stat(k)
    put_branch(a, st)

obs = list(OBS.get(BUILD_ID, []))
for off, oid, ow, why in obs:
    w = struct.unpack_from('<I', IMG, off)[0]
    assert f'{w:08x}' == ow, f'观测门原指令不符 {off:#x}：{w:08x}'
    # 只许函数入口：dm_gate 只回写 x0–x8 / x30，宿主 C 代码可能改 x9–x17；入口处这些按 AAPCS64 不承载值（参数只在 x0–x7 / x8）
    # Function entries only: dm_gate writes back only x0–x8 / x30, and host C code may modify x9–x17; at entry these carry no values under AAPCS64 (arguments are only in x0–x7 / x8)
    assert off in set(starts), f'obs point is not a function entry {off:#x}'
    assert off < TEXT_END and off not in kinds and not any(lo <= off < hi for wseg in windows for lo, hi in [(wseg[0].address, wseg[-1].address + 4)]), f'观测门与改写点冲突 {off:#x}'
    put_branch(off, site_obs(off, oid, w)); stat('obs')
# ---- Parameter slots (host only): {NSO build ID: [(offset A, original word A, offset B, original word B, register, key, reason)]}.
# The two constant-building instructions at the original site are changed so that the first jumps to a parameter trampoline in the patch area and the second is a nop; the trampoline fetches the key page value indirectly and jumps back to just after the first (A+4): when A and B are not adjacent, the original instructions in between execute as usual.
# xR must not be read or written between A and B (offline assertion: the instructions in between do not contain xR).
# The upstream slots (Diablo II: Resurrected builds) are not carried: Tears of the Kingdom registers none ----
PARAMS = {}
params = PARAMS.get(BUILD_ID, [])
for oa, wa, ob_, wb, r, key, why in params:
    for o, w0 in ((oa, wa), (ob_, wb)):
        w = struct.unpack_from('<I', IMG, o)[0]
        assert f'{w:08x}' == w0, f'参数位原指令不符 {o:#x}：{w:08x}'
        assert o not in kinds and not any(lo <= o < hi for wseg in windows for lo, hi in [(wseg[0].address, wseg[-1].address + 4)]), f'参数位与改写点冲突 {o:#x}'
    for o in range(oa + 4, ob_, 4):
        ins = dis(struct.unpack_from('<I', IMG, o)[0], o)
        assert r not in regs_in(ins), f'参数位中间用到 x{r}：{o:#x} {ins.mnemonic} {ins.op_str}'
    start = here()
    tmp = 16 if r != 16 else 17
    emit(STUR(tmp, 31, -16)); emit_host_tp(r); emit_slot_addr(r, tmp, key); emit(LDRX(r, r)); emit(LDUR(tmp, 31, -16)); emit(B(here(), oa + 4))
    struct.pack_into('<I', IMG, oa, B(oa, start)); struct.pack_into('<I', IMG, ob_, NOP); stat('param')

# ---- Spin waits (registered builds) ----
# Ported from nxnative (https://github.com/seedds/nxnative, MIT, commits 066f2af and 6d34984 "Spin waits that take a spin lock";
# games/totk/third_party/licenses/nxnative.txt). Changes for this rewriter: it runs only for the builds in SPIN_BUILDS (each
# version is measured before its loops are gated); there is no data-in-text table here, so a region must lie inside one
# function of the eh_frame list (func_of); the words other rewrites changed are found by comparing with the image as decoded
# (IMG0), plus the windows, call windows and jump tables; the gate is our runtime_gate (gate.S keeps every register and the
# flags) and the host side is runtime_spin_wait (threads.c), which hands the virtual core to a waiting thread.
SPIN_BUILDS = {
    'cd57b23fa4bbad65803d9788c01821ee': 'Breath of the Wild 1.9.0 main (job queue poll main+0xef6354, issue #480)',
}
SPIN_WIN, SPIN_TOPS = 512, {0x16, 0x17, 0x54, 0x34, 0x35, 0xB4, 0xB5, 0x36, 0x37, 0xB6, 0xB7}
spin_cand = {}
spin_sites = []
if BUILD_ID in SPIN_BUILDS:
  for o in range(0, TEXT_END, 4):
    w = struct.unpack_from('<I', IMG, o)[0]
    if w >> 24 in SPIN_TOPS:
        t = w >> 24
        if t in (0x16, 0x17): dd = ((w & 0x3FFFFFF) - (1 << 26)) * 4
        elif t & 0x7E == 0x36: dd = (((w >> 5) & 0x3FFF) - (1 << 14)) * 4 if w & 0x40000 else 4
        elif t == 0x54: dd = (((w >> 5) & 0x7FFFF) - (1 << 19)) * 4 if w & 0x800000 and not w & 0x10 else 4
        else: dd = (((w >> 5) & 0x7FFFF) - (1 << 19)) * 4 if w & 0x800000 else 4
        if -SPIN_WIN <= dd <= 0: spin_cand.setdefault(o + dd, []).append(o)
# A guest loop that polls memory until another thread changes it fills only its own core on the Switch, where any higher-priority thread pinned there preempts it, so the
# spin costs the game nothing; on the host it takes a whole core from the threads doing the work. Paper Mario: The Thousand-Year Door's gfx::ParallelManager workers
# (main+0xa8e680, three, pinned at priority 45) poll their job list this way while a batch is pending; on an iPhone Rogueport ran at 24 fps with the three at 94% of a core each.
# A loop counts as a spin wait only when it provably cannot leave by itself. From its head T, every pass along the region R (the paths from T back to T, within SPIN_WIN bytes
# after T) runs only loads, register and flag arithmetic, direct branches and barriers: no store, call, SVC, system register or event wait; and no register or flag that is
# live at T is written in R (no counter, cursor or delay). One exception: an exclusive add of a constant (ld(a)xr; add/sub #k; st(l)xr; cbnz) that every path back to T
# undoes on the same address (a worker that registers with a job, sees it finished and backs out). Three more let a poll that takes a spin lock to look at the state it waits
# for count too (spin_lock, spin_store): a lock taken (an exclusive test-and-set retried until its store succeeds) and released (an exclusive compare-and-clear, or a store-release
# of zero) on the same address on every path back to T; a test loop on a word (load, compare with a constant, branch back to the load) inside R; and stores whose value and
# address registers R never writes, at most SPIN_STORES of them and only beside a load (see below). Every loop the search took without these still counts as before, the
# same region over the same back-edges or a wider one over more. Breath of the Wild's job queue (main+0xef62dc) is such a poll: the first item of some jobs is for the
# thread of core 0, and the others wait until it is taken, taking and releasing the queue's lock on every pass; polled at full speed by two workers, the lock went back and
# forth between their cores, and the main thread, whose item it was, lost nearly every exclusive store to them (CPU frames of 30-200 ms with three cores spinning there).
# Such a pass leaves memory and registers as it found them (a pass with stores, from its last state change on), so only another thread can end the loop, and a pause
# there changes only timing. R is copied into the patch area; the original back-edges jump to the copy (no pause on the first pass, so a
# single retry costs nothing), the copy's back-edges go through a spin gate, which the host takes as a yield with the yield back-off (hle_thread.c dm_spin_wait), and exits
# from the copy go back to the original code. The gate keeps every register and the flags (entry.S)
SPIN_PATHS, SPIN_STEPS, SPIN_STORES = 256, 4096, 4   # complete paths back to T; search steps (R itself lies within SPIN_WIN bytes)
# Stores in a region: each writes a fixed value to a fixed address, so the words they write can hold only their first values or the stored ones, and the registers at T are the
# same on every pass: with at most SPIN_STORES of them a loop can change its own state only a few hundred passes at most (each a cycle of a few instructions), then its passes
# repeat, and only another thread ends it. The gate polls on for its first 2 ms, so a loop that ends by itself never sleeps there
SPIN_LSE = ('ldadd', 'ldclr', 'ldeor', 'ldset', 'ldsmax', 'ldsmin', 'ldumax', 'ldumin')
SPIN_BAD = {'bl', 'blr', 'br', 'ret', 'eret', 'svc', 'hvc', 'smc', 'brk', 'hlt', 'udf', 'mrs', 'msr', 'sys', 'sysl', 'wfe', 'wfi', 'sev', 'sevl', 'clrex', 'dc', 'ic', 'at',
            'tlbi', 'drps', 'dcps1', 'dcps2', 'dcps3', 'blraa', 'blrab', 'blraaz', 'blrabz', 'braa', 'brab', 'braaz', 'brabz', 'retaa', 'retab', 'eretaa', 'eretab'}
spin_dec = {}
def spin_ins(a):
    if a not in spin_dec: spin_dec[a] = next(md.disasm(bytes(IMG[a:a + 4]), a), None)
    return spin_dec[a]
def spin_norm(n):
    n = {'fp': 'x29', 'lr': 'x30', 'wsp': 'sp'}.get(n, n)
    if n in ('wzr', 'xzr'): return None
    m = re.fullmatch(r'[wx](\d+)', n)
    if m: return 'x' + m.group(1)
    m = re.fullmatch(r'[bhsdqv](\d+)', n)
    return 'v' + m.group(1) if m else n
def spin_rw(ins):
    """(registers read, registers written), w / x and the vector views merged. Capstone's own lists are not enough: it lists a compare's first operand as written and the flags
    as read by cbz / tbz, and leaves out the destination of adds (sdk+0x31033c in Paper Mario: The Thousand-Year Door, adds w9,w9,#8; b.ne: a counted loop). So every register
    operand counts as well: operand 0 written and the others read, or for a load all of them written (its address registers are in capstone's reads)"""
    m = ins.mnemonic
    regs = [spin_norm(ins.reg_name(o.reg)) for o in ins.operands if o.type == ARM64_OP_REG]
    ops = set(regs) - {None}
    if m in ('cbz', 'cbnz', 'tbz', 'tbnz'): return ops, set()
    if m.startswith('b.'): return {'nzcv'}, set()
    if m == 'b': return set(), set()
    if m in ('cmp', 'cmn', 'tst', 'fcmp', 'fcmpe'): return ops, {'nzcv'}
    if m in ('ccmp', 'ccmn', 'fccmp', 'fccmpe'): return ops | {'nzcv'}, {'nzcv'}
    r, w = ins.regs_access()
    r, w = {spin_norm(ins.reg_name(x)) for x in r} - {None}, {spin_norm(ins.reg_name(x)) for x in w} - {None}
    if m.startswith(('ld', 'prfm', 'prfum')): return r, w | ops
    return r | (set(regs[1:]) - {None}), w | (set(regs[:1]) - {None})
def spin_kind(ins):
    """'br' direct branch, 'pc' adr / adrp, 'ok' a load or register / flag arithmetic, None anything else"""
    if ins is None: return None
    m = ins.mnemonic
    if m in ('b', 'cbz', 'cbnz', 'tbz', 'tbnz') or m.startswith('b.'): return 'br'
    if m in ('adr', 'adrp'): return 'pc'
    if m in SPIN_BAD or m.startswith(('st', 'cas', 'swp') + SPIN_LSE): return None
    mem = any(o.type == ARM64_OP_MEM for o in ins.operands)
    if m.startswith(('ld', 'prfm', 'prfum')): return 'ok' if mem else None   # no memory operand: a pc-relative literal load
    return None if mem else 'ok'
def spin_rmw(a):
    """ld(a)xr wA,[xN]; add|sub wB,wA,#k; st(l)xr wS,wB,[xN]; cbnz wS,<the load> at a → (xN, ±k, registers written, the four instructions), or None"""
    l = [spin_ins(a + 4 * i) for i in range(4)]
    if None in l or l[0].mnemonic not in ('ldxr', 'ldaxr') or l[1].mnemonic not in ('add', 'sub') or l[2].mnemonic not in ('stxr', 'stlxr') or l[3].mnemonic != 'cbnz': return None
    r = lambda x, i: x.reg_name(x.operands[i].reg)
    if len(l[1].operands) != 3 or l[1].operands[2].type != ARM64_OP_IMM or 'lsl' in l[1].op_str: return None
    base = l[0].operands[1].mem
    if base.disp or base.index or l[2].operands[2].mem.base != base.base or l[2].operands[2].mem.disp: return None
    if r(l[1], 1) != r(l[0], 0) or r(l[2], 1) != r(l[1], 0) or r(l[3], 0) != r(l[2], 0) or l[3].operands[1].imm != a: return None
    k = l[1].operands[2].imm * (1 if l[1].mnemonic == 'add' else -1)
    return spin_norm(l[0].reg_name(base.base)), k, {spin_norm(r(l[0], 0)), spin_norm(r(l[1], 0)), spin_norm(r(l[2], 0))} - {None}, l
def spin_lock(a):
    """An exclusive spin lock sequence or a test loop entered at a → (kind, xN, its instructions, how many of them come before a, how many from a on, registers read, registers written)
    'acq'  ld(a)xr wA,[xN]; cbz wA,+16; clrex; b <the load>; st(l)xr wS,wK,[xN]; cbnz wS,<the load>    test-and-set, retried until the store succeeds (wK not zero)
    'rel'  clrex; ld(a)xr wA,[xN]; cmp wA,#k; b.ne <the clrex>; st(l)xr wS,wzr,[xN]; cbnz wS,<the load>  compare-and-clear, entered at the load; or stlr wzr,[xN]
    'tst'  ldr|ldrb|ldrh wA,[xN{,#imm}]; cmp wA,#k; b.eq|b.ne <the load>                                 a test loop that only another thread ends"""
    l = [spin_ins(a + 4 * i) for i in range(6)]
    if l[0] is None: return None
    r = lambda x, i: x.reg_name(x.operands[i].reg)
    plain = lambda x, i: x.operands[i].type == ARM64_OP_MEM and not x.operands[i].mem.index and not x.operands[i].mem.disp
    m0 = l[0].mnemonic
    if m0 in ('ldxr', 'ldaxr') and None not in l and r(l[0], 0).startswith('w') and plain(l[0], 1):
        base = l[0].operands[1].mem.base; bn = spin_norm(l[0].reg_name(base))
        if (l[1].mnemonic == 'cbz' and r(l[1], 0) == r(l[0], 0) and l[1].operands[1].imm == a + 16 and l[2].mnemonic == 'clrex' and l[3].mnemonic == 'b' and l[3].operands[0].imm == a
                and l[4].mnemonic in ('stxr', 'stlxr') and plain(l[4], 2) and l[4].operands[2].mem.base == base and r(l[4], 1) not in ('wzr', 'xzr')
                and l[5].mnemonic == 'cbnz' and r(l[5], 0) == r(l[4], 0) and l[5].operands[1].imm == a):
            return ('acq', bn, l, 0, 6, {bn, spin_norm(r(l[4], 1))}, {spin_norm(r(l[0], 0)), spin_norm(r(l[4], 0))})
        p = spin_ins(a - 4)
        if (p is not None and p.mnemonic == 'clrex' and l[1].mnemonic == 'cmp' and r(l[1], 0) == r(l[0], 0) and l[1].operands[1].type == ARM64_OP_IMM
                and l[2].mnemonic == 'b.ne' and l[2].operands[0].imm == a - 4 and l[3].mnemonic in ('stxr', 'stlxr') and r(l[3], 1) == 'wzr'
                and plain(l[3], 2) and l[3].operands[2].mem.base == base and l[4].mnemonic == 'cbnz' and r(l[4], 0) == r(l[3], 0) and l[4].operands[1].imm == a):
            return ('rel', bn, [p] + l[:5], 1, 5, {bn}, {spin_norm(r(l[0], 0)), spin_norm(r(l[3], 0)), 'nzcv'})
    if m0 == 'stlr' and len(l[0].operands) == 2 and r(l[0], 0) in ('wzr', 'xzr') and plain(l[0], 1):
        bn = spin_norm(l[0].reg_name(l[0].operands[1].mem.base)); return ('rel', bn, l[:1], 0, 1, {bn}, set())
    if (m0 in ('ldr', 'ldrb', 'ldrh') and None not in l[:3] and len(l[0].operands) == 2 and r(l[0], 0).startswith('w') and l[0].operands[1].type == ARM64_OP_MEM
            and not l[0].operands[1].mem.index and not l[0].writeback and l[1].mnemonic == 'cmp' and r(l[1], 0) == r(l[0], 0) and l[1].operands[1].type == ARM64_OP_IMM
            and l[2].mnemonic in ('b.eq', 'b.ne') and l[2].operands[0].imm == a):
        bn = spin_norm(l[0].reg_name(l[0].operands[1].mem.base)); return ('tst', bn, l[:3], 0, 3, {bn}, {spin_norm(r(l[0], 0)), 'nzcv'})
    return None
def spin_store(ins):
    """str / strb / strh / stur / sturb / sturh of a w register or zr to [xN{,#imm}] (no writeback, no register offset) → (value register or None for zr, base register), or None"""
    if ins is None or ins.mnemonic not in ('str', 'strb', 'strh', 'stur', 'sturb', 'sturh') or ins.writeback or len(ins.operands) != 2: return None
    v, o = ins.operands
    if v.type != ARM64_OP_REG or o.type != ARM64_OP_MEM or o.mem.index: return None
    vn = ins.reg_name(v.reg)
    if not (vn.startswith('w') or vn in ('xzr',)): return None
    return (spin_norm(vn), spin_norm(ins.reg_name(o.mem.base)))
def spin_rebranch(ins, pc, tgt):
    """the direct branch ins re-encoded at pc with target tgt"""
    w = struct.unpack('<I', ins.bytes)[0]; off = tgt - pc
    if ins.mnemonic == 'b': return B(pc, tgt)
    if ins.mnemonic in ('tbz', 'tbnz'):
        assert -(1 << 15) <= off < (1 << 15) and off % 4 == 0, f'spin wait tbz out of range {pc:#x}'
        return (w & ~(0x3FFF << 5)) | (((off >> 2) & 0x3FFF) << 5)
    assert -(1 << 20) <= off < (1 << 20) and off % 4 == 0, f'spin wait branch out of range {pc:#x}'
    return (w & ~(0x7FFFF << 5)) | (((off >> 2) & 0x7FFFF) << 5)
spin_touched = ((set(kinds) | done | {i.address for wseg in windows for i in wseg}   # sites, windows and call windows (done), and
                 | {o for o in range(0, TEXT_END, 4) if IMG[o:o + 4] != IMG0[o:o + 4]}) if spin_cand else set())   # every word another rewrite changed
# Instruction words that end every path through them, told apart by their encoding alone: bl; br / blr / ret; svc / brk; str / strb / strh and the vector str (unsigned offset;
# unscaled, pre / post-index; register offset); stp / stnp; ldadd and the other LSE atomics. Exclusive stores are left to the full check (the exclusive add), and so are the pre /
# post-index loads: a seqlock read in SDK 15's nn::time (sdk+0x3d3270) moves a base it sets again on each pass
SPIN_STOP = ((0xFC000000, 0x94000000), (0xFE000000, 0xD6000000), (0xFF000000, 0xD4000000), (0x3FC00000, 0x39000000), (0x3FC00000, 0x3D000000), (0x3FC00000, 0x3D800000),
             (0x3FE00000, 0x38000000), (0x3FE00C00, 0x38200800), (0x3FE00000, 0x3C000000), (0x3FE00000, 0x3C800000), (0x3A400000, 0x28000000), (0x3F200C00, 0x38200000))
def spin_quick(T, hi):
    """a back-edge to T can be reached from T without passing a word of SPIN_STOP (only such heads get the full check, which decodes)"""
    seen, todo = set(), [T]
    while todo:
        a = todo.pop()
        if a in seen or not T <= a <= hi: continue
        seen.add(a); w = struct.unpack_from('<I', IMG, a)[0]; t = w >> 24
        if 0x14 <= t <= 0x17: succ = (a + (((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000) * 4,)
        elif t == 0x54 or t & 0x7E == 0x34: succ = (a + ((((w >> 5) & 0x7FFFF) ^ 0x40000) - 0x40000) * 4, a + 4)
        elif t & 0x7E == 0x36: succ = (a + ((((w >> 5) & 0x3FFF) ^ 0x2000) - 0x2000) * 4, a + 4)
        elif any(w & m == v for m, v in SPIN_STOP) and not (w & 0xBFC00000 in (0xB9000000, 0x39000000, 0x79000000) or w & 0xBFE00C00 in (0xB8000000, 0x38000000, 0x78000000)): continue   # str / strb / strh (immediate, unscaled) may store the same on every pass
        else: succ = (a + 4,)
        if T in succ: return True
        todo.extend(succ)
    return False
class SpinNo(Exception): pass
def spin_region(T, bes):
    """the spin wait region with head T (bes: the backward branches to T found by the scan) → dict, or None. A store whose value or address a pass changes is no part of a region:
    the paths through it are dropped and the region is found again without them (a poll that takes its item once the wait is over stores the new cursor there). When the paths
    through stores bring in a register that a pass changes, the region is looked for once more with every store ending its path, as before stores could be part of one"""
    ban = set()
    for _ in range(8):
        g = spin_region1(T, bes, ban, True)
        if not isinstance(g, set): break
        ban |= g
    old = spin_region1(T, bes, set(), False)   # the region without stores, which the search took before: the wider one only when it covers its back-edges
    return g if isinstance(g, dict) and (not old or old['valid'] <= g['valid']) else old
def spin_region1(T, bes, ban, stores):
    hi = max(bes)
    fs, fe = func_of(T)
    if not (fs <= T and hi + 4 <= fe) or not spin_quick(T, hi): return None   # inside one function (no data-in-text table here)
    nodes, back, bad, inner = {}, set(), set(), set()
    todo = [T]
    while todo:
        a = todo.pop()
        if a in nodes or a in bad: continue
        if a in ban: bad.add(a); continue
        rmw = spin_rmw(a) if a + 12 <= hi and struct.unpack_from('<I', IMG, a)[0] & 0x3FE00000 == 0x08400000 else None   # ldxr / ldaxr
        lk = None if rmw else spin_lock(a)
        if lk and lk[0] == 'tst' and a == T: lk = None   # the region's own test loop
        if rmw:
            n = {'kind': 'rmw', 'base': rmw[0], 'k': rmw[1], 'ins': rmw[3], 'uses': {rmw[0]}, 'defs': rmw[2], 'succ': [a + 16]}
        elif lk:
            n = {'kind': lk[0], 'base': lk[1], 'k': {'acq': 1, 'rel': -1, 'tst': 0}[lk[0]], 'ins': lk[2], 'pre': lk[3], 'uses': lk[5], 'defs': lk[6], 'succ': [a + 4 * lk[4]]}
        elif stores and spin_store(spin_ins(a)):
            x = spin_ins(a); st = spin_store(x); n = {'kind': 'stc', 'st': st, 'ins': [x], 'uses': {st[1]} | ({st[0]} - {None}), 'defs': set(), 'succ': [a + 4]}
        else:
            x = spin_ins(a); k = spin_kind(x)
            if k is None: bad.add(a); continue
            if k == 'br':
                t = x.operands[-1].imm
                succ = [t] if x.mnemonic == 'b' else [t, a + 4]
            else: succ = [a + 4]
            n = {'kind': k, 'ins': [x], 'succ': succ}
        pre = n.get('pre', 0)
        if any(a + 4 * (i - pre) in spin_touched for i in range(len(n['ins']))): bad.add(a); continue
        nodes[a] = n
        if n['kind'] == 'rmw': inner |= {a + 4, a + 8, a + 12}
        if n['kind'] in ('acq', 'rel', 'tst'): inner |= {a + 4 * (i - pre) for i in range(len(n['ins']))} - {a}
        for s in n['succ']:
            if s == T: back.add(a)
            elif T < s <= hi: todo.append(s)
    if not back or inner & set(nodes) or any(s in inner for n in nodes.values() for s in n['succ']): return None
    def prune(ends):
        keep, more = set(ends), True
        while more:
            more = False
            for a, n in nodes.items():
                if a not in keep and any(s in keep and s != T for s in n['succ']): keep.add(a); more = True
        return keep
    R = prune(back)
    for a in R:
        if 'uses' not in nodes[a]: nodes[a]['uses'], nodes[a]['defs'] = spin_rw(nodes[a]['ins'][0])
    # every path from T to a back-edge: no inner cycle, and the exclusive adds on each address (a register between two writes to it) sum to 0
    bad_back, cnt = set(), [0, 0]
    def walk(a, onpath, ver, delta):
        cnt[1] += 1
        if cnt[1] > SPIN_STEPS: raise SpinNo
        n = nodes[a]
        if n['kind'] in ('rmw', 'acq', 'rel'):   # exclusive adds and lock / unlock pairs, each kind on its own
            key = (n['kind'] != 'rmw', n['base'], ver.get(n['base'], 0)); delta = dict(delta); delta[key] = delta.get(key, 0) + n['k']
            if n['kind'] != 'rmw' and not 0 <= delta[key] <= 1: raise SpinNo   # a release before its acquire or a second acquire: not a lock this pass takes once
        if n['defs']: ver = dict(ver); ver.update({r: ver.get(r, 0) + 1 for r in n['defs']})
        for s in n['succ']:
            if s == T:
                cnt[0] += 1
                if cnt[0] > SPIN_PATHS: raise SpinNo
                if any(delta.values()): bad_back.add(a)
            elif s in R:
                if s in onpath: raise SpinNo
                walk(s, onpath | {s}, ver, delta)
    try: walk(T, {T}, {}, {})
    except SpinNo: return None
    valid = back - bad_back
    if not valid: return None
    R = prune(valid)
    # no state carried from one pass to the next: nothing live at T is written in R
    live, more = {a: set() for a in R}, True
    while more:
        more = False
        for a in sorted(R, reverse=True):
            n, out = nodes[a], set()
            for s in n['succ']:
                if s == T: out |= live[T] if a in valid else set()
                elif s in R: out |= live[s]
            new = n['uses'] | (out - n['defs'])
            if new != live[a]: live[a] = new; more = True
    defs = set().union(*(nodes[a]['defs'] for a in R))
    if any(nodes[a]['kind'] == 'stc' for a in R) and not any(nodes[a]['kind'] in ('rmw', 'acq', 'rel', 'tst') or nodes[a]['ins'][0].mnemonic.startswith('ld') for a in R): return None   # stores and nothing polled
    vary = {a for a in R if nodes[a]['kind'] == 'stc' and nodes[a]['uses'] & defs}   # a store's value and address must come from registers no pass writes
    if vary: return vary
    if sum(nodes[a]['kind'] == 'stc' for a in R) > SPIN_STORES: return None
    if live[T] & defs: return None
    return {'T': T, 'R': sorted(R), 'nodes': nodes, 'valid': valid}
def spin_emit(g):
    T, R, nodes, valid = g['T'], g['R'], g['nodes'], g['valid']
    cpy, fixes, tramp = {}, [], {}
    for i, a in enumerate(R):
        n = nodes[a]; cpy[a] = here(); x = n['ins'][0]
        if n['kind'] == 'rmw':
            for y in n['ins'][:3]: emit(struct.unpack('<I', y.bytes)[0])
            fixes.append((len(patch), n['ins'][3], a)); emit(0)         # retry: the copy's exclusive load
        elif n['kind'] in ('acq', 'rel'):   # copied as they are: their branches stay inside the sequence
            cpy[a] = here() + 4 * n['pre']
            for y in n['ins']: emit(struct.unpack('<I', y.bytes)[0])
        elif n['kind'] == 'tst':   # its branch back goes to the original loop, which passes its own spin gate when it is a spin wait itself
            for y in n['ins'][:2]: emit(struct.unpack('<I', y.bytes)[0])
            fixes.append((len(patch), n['ins'][2], ('orig', a))); emit(0)
        elif n['kind'] == 'pc':
            rd, tgt = num(spin_norm(x.reg_name(x.operands[0].reg))), x.operands[1].imm
            emit(ADRP(rd, here(), tgt))
            if x.mnemonic == 'adr': emit(ADDI(rd, rd, tgt & 0xFFF))
        elif n['kind'] == 'br': fixes.append((len(patch), x, None)); emit(0)
        else: emit(struct.unpack('<I', x.bytes)[0])
        if not (n['kind'] == 'br' and x.mnemonic == 'b'):
            ft = a + 4 * (len(n['ins']) - n.get('pre', 0))
            if not (i + 1 < len(R) and R[i + 1] == ft): fixes.append((len(patch), None, ft)); emit(0)   # the next instruction is not the next one copied
    pause = here()
    emit(STP_PRE(29, 30, 31, -16)); emit_gate_call(); emit(BLR(30)); ret = here(); emit(LDP_POST(29, 30, 31, 16))
    gates.append((ret, 'spin', 0, T)); emit(B(here(), cpy[T]))
    def out(t):   # an exit taken by a conditional branch: through a b next to the copy
        if t not in tramp: tramp[t] = here(); emit(B(here(), t))
        return tramp[t]
    for idx, x, info in fixes:
        pc = PATCH_OFF + idx
        if x is None: w = B(pc, cpy.get(info, info))
        elif isinstance(info, tuple): w = spin_rebranch(x, pc, out(info[1]))
        elif info is not None: w = spin_rebranch(x, pc, cpy[info])
        else:
            t, cond = x.operands[-1].imm, x.mnemonic != 'b'
            d = pause if t == T and x.address in valid else cpy[t] if t in cpy and t != T else out(t) if cond else t
            w = spin_rebranch(x, pc, d)
        struct.pack_into('<I', patch, idx, w)
    for b in sorted(valid):   # the original back-edges enter the copy
        x = nodes[b]['ins'][0]
        if x.mnemonic == 'b': put_branch(b, cpy[T])
        else: st = here(); emit(spin_rebranch(x, here(), cpy[T])); emit(B(here(), b + 4)); put_branch(b, st)
    stat('spin')
spin_regions = [g for g in (spin_region(T, spin_cand[T]) for T in sorted(spin_cand)) if g]   # all found on the unchanged image first: emitting patches back-edges
spin_sites = [(g['T'], sorted(g['valid']), sum(len(g['nodes'][a]['ins']) for a in g['R'])) for g in spin_regions]
for g in spin_regions: spin_emit(g)
if os.environ.get('DM_SPIN_LIST'):
    for T, bes, n in spin_sites: print(f'{NAME} spin wait {T:#x} ({n} instructions), back-edges ' + ' '.join(f'{b:#x}' for b in bes))

os.makedirs(outdir, exist_ok=True)
P = lambda ext: os.path.join(outdir, f'{NAME}.{ext}')
open(P('patched.bin'), 'wb').write(IMG + bytearray(PATCH_OFF - len(IMG)) + patch)
with open(os.path.join(outdir, 'build_src.sha256'), 'w') as f:
    f.write(hashlib.sha256(open(os.path.abspath(__file__), 'rb').read()).hexdigest() + '  patch_nso.py\n')
with open(P('fixups.tsv'), 'w') as f:
    f.write(f'# PATCH_OFF\t{PATCH_OFF:#x}\n# KEY_PAGE_OFF\t{KEY_PAGE_OFF:#x}\n# KEY_PAGE_SIZE\t0x4000\n# TEXT_END\t{TEXT_END:#x}\n# RO_OFF\t{RO_OFF:#x}\n# DATA_OFF\t{DATA_OFF:#x}\n')
    for k in KEY_INDEX: f.write(f'# KEY_{k}_IDX\t{KEY_INDEX[k]}\n')
with open(P('gates.tsv'), 'w') as f:
    for ret, k, arg, orig in gates: f.write(f'{ret:#x}\t{k}\t{arg}\t{orig:#x}\n')
with open(P('sites.tsv'), 'w') as f:
    for a in sorted(kinds):
        if a not in done: f.write(f'{a:#x}\t{kinds[a][0]}\n')
    for wseg in windows: f.write(f'{wseg[0].address:#x}\tW\t{wseg[-1].address + 4:#x}\n')
    for prefix, (call, _) in sorted(calls.items()): f.write(f'{prefix:#x}\tC\t{call + 4:#x}\n')
    for a, (b, c) in jumps.items(): f.write(f'{a:#x}\tJ\t{b:#x}\t{c}\n')
    for t, bes, n in spin_sites: f.write(f'{t:#x}\tS\t' + ' '.join(f'{b:#x}' for b in bes) + '\n')   # spin wait: its head and the back-edges that enter its copy
with open(P('cuts.tsv'), 'w') as f:
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
    for off, ob, nb, why, scope in cuts: f.write(f'{off:#x}\t{ob}\t{nb}\t{scope}\t{why}\n')   # 列：偏移 原字节 新字节 作用域 理由 | Columns: offset, original bytes, new bytes, scope, reason
with open(P('params.tsv'), 'w') as f:
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
    for oa, wa, ob_, wb, r, key, why in params: f.write(f'{oa:#x}\t{ob_:#x}\tx{r}\t{key}\t{why}\n')
with open(P('obs.tsv'), 'w') as f:
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
    for off, oid, ow, why in obs: f.write(f'{off:#x}\t{oid}\t{ow}\t{why}\n')
print(NAME, stats, 'cuts', len(cuts), 'obs', len(obs), 'patch', len(patch), 'PATCH_OFF', hex(PATCH_OFF), 'slot_refs', len(fix), 'gates', len(gates))
# 启动块清单随 main 出：取仓库版本化数据 dm/data/bootlist_<main 构建号>.tsv 原样拷进产物目录并写 sha256（清单入库、不每版重录；
# 记录本身有竞态——同产物三次记录块集合 5292 / 5114 / 5281、发起线程亦有不同——重录只在构建号变化或主动改进时做，提交附 mini A/B）
# The boot block list ships with main: take the repository's versioned data dm/data/bootlist_<main build ID>.tsv, copy it as is into the build output directory and write its sha256 (the list is checked in, not re-recorded for every version;
# the recording itself is racy -- three recordings of the same build output gave block sets of 5292 / 5114 / 5281, and the initiating threads also differed -- re-record only when the build ID changes or for a deliberate improvement, and attach an A/B from the second Mac to the commit)
if NAME == 'main':
    import shutil
    BL = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'data', f'bootlist_{BUILD_ID}.tsv')
    if not os.path.isfile(BL):   # 公开仓库不带这份清单（它每行带原版资源块的开头字节）：没有就不预取，启动时少一步提前解码，别的不受影响 | the public repository does not ship this list (each line carries the first bytes of an original resource block): without it there is no prefetch, so launch skips one ahead-of-time decode step and nothing else is affected
        print('bootlist 没有启动块清单，跳过（不预取）。要用的话自己录一份：dm/tools/tact_bootlist.py')
    else:
        hd = open(BL, encoding='utf-8').readline().split()
        if hd[-1:] != [BUILD_ID]: sys.exit(f'【红】{BL} 头行构建号 {hd[-1:]} 与 main {BUILD_ID} 不符')
        shutil.copyfile(BL, os.path.join(outdir, 'bootlist.tsv'))
        sha = hashlib.sha256(open(BL, 'rb').read()).hexdigest()
        open(os.path.join(outdir, 'bootlist.sha256'), 'w').write(f'{sha}  bootlist.tsv\n')
        print('bootlist', os.path.relpath(BL), sha[:12])
