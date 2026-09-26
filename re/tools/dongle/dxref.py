"""usage: python re/tools/dongle/dxref.py 0xVALUE [mask]  -- find literal-pool words == value (optionally value&mask) and their LDR users"""
import struct,sys,re
img=open('dump/dongle/flash.bin','rb').read()
v=int(sys.argv[1],16); m=int(sys.argv[2],16) if len(sys.argv)>2 else 0xffffffff
lst=open('re/tools/dongle/app.lst').read().splitlines()+open('re/tools/dongle/stub.lst').read().splitlines()
pools=[a for a in range(0,0xb000,4) if (struct.unpack_from('<I',img,a)[0]&m)==v]
for p in pools:
    w=struct.unpack_from('<I',img,p)[0]
    users=[l for l in lst if f'; [{p:05x}]' in l]
    print(f'pool 0x{p:05x}=0x{w:08x}: '+(' | '.join(u.split(':')[0] for u in users)))
