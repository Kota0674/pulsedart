"""usage: python re/tools/dongle/dcallers.py 0xT [..] -- BL/B.W callers and thumb pointers"""
import struct,sys
img=open('dump/dongle/flash.bin','rb').read()
tg=set(int(a,16) for a in sys.argv[1:])
for lo,hi in ((0,0x4050),(0x5000,0xa9e0)):
  for a in range(lo,hi-3,2):
    h1,h2=struct.unpack_from('<HH',img,a)
    if (h1&0xF800)==0xF000 and (h2&0xD000) in (0xD000,0x9000):
      S=(h1>>10)&1; imm10=h1&0x3ff; J1=(h2>>13)&1; J2=(h2>>11)&1; imm11=h2&0x7ff
      I1=1-(J1^S); I2=1-(J2^S)
      off=(S<<24)|(I1<<23)|(I2<<22)|(imm10<<12)|(imm11<<1)
      if S: off-=1<<25
      t=a+4+off
      if t in tg: print(f'{"bl" if h2&0x4000 else "b.w"} @0x{a:05x} -> 0x{t:05x}')
  for a in range(lo,hi-3,4):
    v=struct.unpack_from('<I',img,a)[0]
    if (v&~1) in tg and v&1: print(f'ptr @0x{a:05x} = 0x{v:08x}')
