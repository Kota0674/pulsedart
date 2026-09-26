"""Find BL/B.W callers of given targets in app. usage: python re/tools/blrefs.py 0x5fe0e [0x...]"""
import sys, struct
sys.path = [p for p in sys.path if not p.endswith('tools')]
img=open('dump/flash.bin','rb').read()
tg={int(a,16) for a in sys.argv[1:]}
for a in range(0x50000,0x68000,2):
    h1,h2=struct.unpack_from('<HH',img,a)
    if (h1>>11)==0b11110 and (h2>>14)==0b11 and (h2>>12)&1:
        S=(h1>>10)&1; imm10=h1&0x3ff; J1=(h2>>13)&1; J2=(h2>>11)&1; imm11=h2&0x7ff
        I1=1-(J1^S); I2=1-(J2^S)
        off=(S<<24)|(I1<<23)|(I2<<22)|(imm10<<12)|(imm11<<1)
        if S: off-=1<<25
        t=a+4+off
        if t in tg: print(f'{a:05x} -> {t:05x}', 'bl' if (h2>>14)&1 else 'b.w')
