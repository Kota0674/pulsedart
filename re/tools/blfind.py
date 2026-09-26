"""Brute-force find BL/B.W (T4) instructions at any halfword offset targeting given addresses.
usage: python re/tools/blfind.py START END target [target...]"""
import struct, sys
img=open('dump/flash.bin','rb').read()
S=int(sys.argv[1],16);E=int(sys.argv[2],16);T=set(int(x,16)&~1 for x in sys.argv[3:])
for a in range(S,E-3,2):
    h1,h2=struct.unpack_from('<HH',img,a)
    if (h1>>11)!=0b11110: continue
    kind=None
    if (h2&0xd000)==0xd000: kind='bl'
    elif (h2&0xd000)==0x9000: kind='b.w'
    elif (h2&0xd000)==0xc000: kind='blx'
    else: continue
    s=(h1>>10)&1; imm10=h1&0x3ff; j1=(h2>>13)&1; j2=(h2>>11)&1; imm11=h2&0x7ff
    i1=1-(j1^s); i2=1-(j2^s)
    off=(s<<24)|(i1<<23)|(i2<<22)|(imm10<<12)|(imm11<<1)
    if s: off-=1<<25
    t=a+4+off
    if kind=='blx': t&=~3
    if t in T: print(f'{a:05x}: {kind} -> {t:05x}')
