"""Find Thumb code that loads a given literal (e.g. a peripheral base) and disassemble around it.
usage: python xref.py dump/flash.bin 0x4001C000 [before] [after]
"""
import struct, sys
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
from capstone.arm import ARM_OP_MEM, ARM_REG_PC

img = open(sys.argv[1], 'rb').read()
target = int(sys.argv[2], 16)
before = int(sys.argv[3]) if len(sys.argv) > 3 else 40
after = int(sys.argv[4]) if len(sys.argv) > 4 else 60

md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
md.detail = True

pools = [i for i in range(0, len(img) - 3, 4) if struct.unpack_from('<I', img, i)[0] == target]

def disasm(start, end):
    return list(md.disasm(img[start:end], start))

def lit(insn):
    """Resolve pc-relative literal loads to their value, for annotation."""
    for op in insn.operands:
        if op.type == ARM_OP_MEM and op.mem.base == ARM_REG_PC:
            a = ((insn.address + 4) & ~3) + op.mem.disp
            if 0 <= a < len(img) - 3:
                return a, struct.unpack_from('<I', img, a)[0]
    return None

seen = set()
for p in pools:
    # an LDR literal reaches at most 4 KB forward; scan code before the pool
    lo = max(0, p - 4096) & ~1
    for off in (0, 2):  # try both halfword alignments to resync the decoder
        for insn in md.disasm(img[lo + off:p], lo + off):
            r = lit(insn)
            if r and r[0] == p and insn.address not in seen:
                seen.add(insn.address)
                print(f"\n==== ref @0x{insn.address:05x} -> literal 0x{p:05x} = 0x{target:08x}")
                s = max(0, insn.address - before * 2)
                # resync: start at the xref and walk back by decoding from a few candidates
                for i in disasm(s, insn.address + after * 4):
                    r2 = lit(i)
                    note = f"   ; =0x{r2[1]:08x}" if r2 else ""
                    mark = ">>" if i.address == insn.address else "  "
                    print(f"{mark}{i.address:05x}: {i.mnemonic:8} {i.op_str}{note}")
                    if i.address > insn.address and i.mnemonic in ("bx", "pop") and "pc" in i.op_str + i.mnemonic and i.mnemonic != "pop" or (i.mnemonic == "pop" and "pc" in i.op_str and i.address > insn.address):
                        break
