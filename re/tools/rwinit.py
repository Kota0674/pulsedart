"""Decompress Keil RW-data (scatter entry 0x66f24: src 0x66fbc -> 0x20002210, len 0x624).
Writes re/tools/rwdata.bin (base 0x20002210). usage: python re/tools/rwinit.py [addr len]"""
import sys, struct
img=open('dump/flash.bin','rb').read()
src,dst,ln=0x66fbc,0x20002210,0x624
out=bytearray(); p=src
while len(out)<ln:
    b=img[p]; p+=1
    n=b&7
    if n==0: n=img[p]; p+=1
    m=b>>4
    if m==0: m=img[p]; p+=1
    for _ in range(n-1): out.append(img[p]); p+=1
    if b&8:
        off=img[p]; p+=1
        s=len(out)-off
        for k in range(m+2): out.append(out[s+k])
    else:
        out+=bytes(m)
open('re/tools/rwdata.bin','wb').write(out[:ln])
print('end src',hex(p),'len',hex(len(out)))
if len(sys.argv)>2:
    a=int(sys.argv[1],16)-dst; l=int(sys.argv[2],16)
    print(out[a:a+l].hex(' '))
