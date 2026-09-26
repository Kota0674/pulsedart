"""Rough function map of dongle app: function starts = BL targets; per function list literals & callees.
usage: python re/tools/dongle/funcs.py [lo hi]"""
import struct,sys,re
img=open('dump/dongle/flash.bin','rb').read()
lst=[l for l in open('re/tools/dongle/app.lst').read().splitlines()]
tg={}
for a in range(0x5000,0xa9e0-3,2):
    h1,h2=struct.unpack_from('<HH',img,a)
    if (h1&0xF800)==0xF000 and (h2&0xD000)==0xD000:
      S=(h1>>10)&1; imm10=h1&0x3ff; J1=(h2>>13)&1; J2=(h2>>11)&1; imm11=h2&0x7ff
      I1=1-(J1^S); I2=1-(J2^S)
      off=(S<<24)|(I1<<23)|(I2<<22)|(imm10<<12)|(imm11<<1)
      if S: off-=1<<25
      t=a+4+off
      if 0x5000<=t<0xa9e0: tg.setdefault(t,[]).append(a)
for a in range(0x5000,0xa9e0,4):
    v=struct.unpack_from('<I',img,a)[0]
    if v&1 and 0x5000<=v-1<0xa9e0: tg.setdefault(v-1,[]).append(('ptr',a))
starts=sorted(tg)
lo=int(sys.argv[1],16) if len(sys.argv)>1 else 0; hi=int(sys.argv[2],16) if len(sys.argv)>2 else 1<<32
idx={int(l[:5],16):l for l in lst}
addrs=sorted(idx)
import bisect
for i,s in enumerate(starts):
    if not lo<=s<hi: continue
    e=starts[i+1] if i+1<len(starts) else 0xa9e0
    lits=set();calls=set()
    for a in addrs[bisect.bisect_left(addrs,s):bisect.bisect_left(addrs,e)]:
        l=idx[a]
        m=re.search(r'=0x([0-9a-f]{8})',l)
        if m:
            v=int(m.group(1),16)
            if v>=0x20000000: lits.add(v)
        m=re.search(r'\bbl\s+#0x([0-9a-f]+)',l)
        if m: calls.add(int(m.group(1),16))
    nc=len(tg[s])
    print(f'{s:05x}-{e:05x} callers={nc} lits={" ".join(hex(x) for x in sorted(lits))} calls={" ".join(hex(x) for x in sorted(calls))}')
