"""Thumb disassembler with literal annotation. usage: python re/tools/sensdis.py START END"""
import struct, sys
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
from capstone.arm import ARM_OP_MEM, ARM_REG_PC
img = open('dump/flash.bin','rb').read()
s=int(sys.argv[1],16); e=int(sys.argv[2],16)
md=Cs(CS_ARCH_ARM, CS_MODE_THUMB); md.detail=True; md.skipdata=True
for i in md.disasm(img[s:e], s):
    note=''
    try:
        for op in i.operands:
            if op.type==ARM_OP_MEM and op.mem.base==ARM_REG_PC:
                a=((i.address+4)&~3)+op.mem.disp
                note=f'   ; [{a:05x}]=0x{struct.unpack_from("<I",img,a)[0]:08x}'
    except Exception: pass
    print(f'{i.address:05x}: {i.bytes.hex():10} {i.mnemonic:8} {i.op_str}{note}')
