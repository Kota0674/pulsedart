"""Dongle (nRF52810) Thumb disassembler with literal annotation.
usage: python re/tools/dongle/ddis.py START END   (hex)   -- from the repository root
       python re/tools/dongle/ddis.py all        -- writes re/tools/dongle/stub.lst and app.lst"""
import struct, sys
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
from capstone.arm import ARM_OP_MEM, ARM_REG_PC
img=open('dump/dongle/flash.bin','rb').read()
md=Cs(CS_ARCH_ARM,CS_MODE_THUMB); md.detail=True; md.skipdata=True
def dis(s,e,out):
    for i in md.disasm(img[s:e],s):
        note=''
        if i.id!=0:
            try:
                for op in i.operands:
                    if op.type==ARM_OP_MEM and op.mem.base==ARM_REG_PC:
                        a=((i.address+4)&~3)+op.mem.disp
                        note=f'   ; [{a:05x}]=0x{struct.unpack_from("<I",img,a)[0]:08x}'
            except Exception: pass
        out.write(f'{i.address:05x}: {i.bytes.hex():10} {i.mnemonic:8} {i.op_str}{note}\n')
if sys.argv[1]=='all':
    dis(0,0x4050,open('re/tools/dongle/stub.lst','w'))
    dis(0x5000,0xa9e0,open('re/tools/dongle/app.lst','w'))
else:
    dis(int(sys.argv[1],16),int(sys.argv[2],16),sys.stdout)
