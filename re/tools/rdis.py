"""Recursive-descent Thumb disassembler for a flash range.
usage: python re/tools/rdis.py START END VECTOR_BASE [extra_entry ...] > out.lst
Seeds: vector table entries, extra entries, then every BL/B.W target and every
literal-pool word that looks like a Thumb function pointer into [START,END).
Output: listing grouped by function start, with literal annotation and xrefs.
"""
import struct, sys
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
from capstone.arm import ARM_OP_MEM, ARM_REG_PC, ARM_OP_IMM

img = open('dump/flash.bin', 'rb').read()
S = int(sys.argv[1], 16); E = int(sys.argv[2], 16); V = int(sys.argv[3], 16)
extra = [int(x, 16) for x in sys.argv[4:]]
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB); md.detail = True

def u32(a): return struct.unpack_from('<I', img, a)[0]

insns = {}      # addr -> insn
lits = set()    # literal pool addresses (4 bytes)
funcs = set()
calls = {}      # target -> [callers]
work = []

def add_entry(a, func=True):
    a &= ~1
    if S <= a < E:
        if func: funcs.add(a)
        work.append(a)

for i in range(16 + 48):
    v = u32(V + 4 * i)
    if v and v != 0xffffffff and v & 1: add_entry(v)
for x in extra: add_entry(x)

def lit_of(i):
    for op in i.operands:
        if op.type == ARM_OP_MEM and op.mem.base == ARM_REG_PC:
            a = ((i.address + 4) & ~3) + op.mem.disp
            return a
    return None

def run():
    while work:
        a = work.pop()
        while S <= a < E and a not in insns and a not in lits:
            try:
                i = next(md.disasm(img[a:a + 4], a, 1))
            except StopIteration:
                break
            insns[a] = i
            la = lit_of(i)
            if la is not None and S <= la < E:
                sz = 4
                if i.mnemonic.startswith(('ldrd',)): sz = 8
                for k in range(0, sz, 4): lits.add(la + k)
                if i.mnemonic.startswith('ldr') and not i.mnemonic.startswith(('ldrb', 'ldrh', 'ldrsb', 'ldrsh')):
                    v = u32(la)
                    if v & 1 and S <= v < E:
                        add_entry(v)
            m = i.mnemonic
            if m == 'adr':
                pass
            if m in ('bl', 'blx') and i.operands and i.operands[0].type == ARM_OP_IMM:
                t = i.operands[0].imm
                calls.setdefault(t, []).append(a)
                add_entry(t)
            elif m.startswith('b') and m not in ('bl', 'blx', 'bx', 'bic', 'bics', 'bfi', 'bfc', 'bkpt') and i.operands and i.operands[0].type == ARM_OP_IMM:
                t = i.operands[0].imm
                work.append(t)
                if m in ('b', 'b.w'):
                    break
            elif m in ('cbz', 'cbnz'):
                work.append(i.operands[1].imm)
            elif m in ('tbb', 'tbh'):
                # jump table follows
                n = None
                # look back for cmp rX,#n
                for back in range(2, 16, 2):
                    p = insns.get(a - back)
                    if p and p.mnemonic.startswith('cmp') and p.operands[1].type == ARM_OP_IMM:
                        n = p.operands[1].imm + 1; break
                if n:
                    base = a + 4
                    for k in range(n):
                        off = img[base + k] if m == 'tbb' else struct.unpack_from('<H', img, base + 2 * k)[0]
                        work.append(base + 2 * off)
                    tsz = n if m == 'tbb' else 2 * n
                    for k in range(0, (tsz + 3) & ~3, 4): lits.add(base + k)  # mark table as data
                break
            elif m == 'bx' or (m.startswith('pop') and 'pc' in i.op_str) or (m.startswith('ldr') and i.op_str.startswith('pc')):
                if m == 'bx' or not m.endswith(('eq','ne','cs','cc','hs','lo','mi','pl','vs','vc','hi','ls','ge','lt','gt','le')):
                    break
            elif m == 'mov' and i.op_str.startswith('pc'):
                break
            a += i.size

run()
# pointer-like literals (function pointer tables in rodata)
for a in range(S, E - 3, 4):
    if a in lits or a in insns: continue
for a in list(lits):
    v = u32(a)
    if v & 1 and S <= v < E and (v & ~1) not in insns:
        add_entry(v)
run()
# prologue scan in uncovered gaps (functions reached only via pointers we did not resolve)
for _ in range(3):
    covered = set()
    for a, i in insns.items():
        for k in range(i.size): covered.add(a + k)
    for a in lits:
        for k in range(4): covered.add(a + k)
    for a in range(S, E - 3, 2):
        if a in covered: continue
        h1, h2 = struct.unpack_from('<HH', img, a)
        if (h1 & 0xff00) == 0xb500 or (h1 == 0xe92d and (h2 & 0x4000)):
            add_entry(a)
    run()

fl = sorted(funcs)
def fname(a): return f"sub_{a:05x}"
out = []
cur = None
for a in sorted(insns):
    if a in funcs:
        xs = calls.get(a, [])
        out.append(f"\n;---------------- {fname(a)}  callers: {' '.join(hex(x) for x in xs[:12])}{' ...' if len(xs) > 12 else ''}")
    i = insns[a]
    note = ''
    la = lit_of(i)
    if la is not None and la + 4 <= len(img):
        v = u32(la)
        note = f'   ; [{la:05x}]=0x{v:08x}'
    if i.mnemonic in ('bl', 'blx') and i.operands[0].type == ARM_OP_IMM:
        note = f'   ; -> {fname(i.operands[0].imm)}'
    if a - 2 not in insns and a - 4 not in insns and a not in funcs:
        out.append('        ...')
    out.append(f"{a:05x}: {i.bytes.hex():10} {i.mnemonic:8} {i.op_str}{note}")
print('\n'.join(out))
print(f"\n; {len(insns)} insns, {len(funcs)} funcs, {len(lits)} literal words", file=sys.stderr)
