"""Decompress ARMCC scatter-load RW data (table @0x66f24) -> re/tools/rw_20002210.bin"""
import struct
d=open('dump/flash.bin','rb').read()
src,dst,size,h=struct.unpack_from('<4I',d,0x66f24)
out=bytearray(); i=src
while len(out)<size:
    t=d[i]; i+=1
    lit=t&7
    if lit==0: lit=d[i]; i+=1
    m=t>>4
    if m==0: m=d[i]; i+=1
    out+=d[i:i+lit-1]; i+=lit-1
    if t&8:
        off=d[i]; i+=1
        for _ in range(m+2): out.append(out[-off])
    else:
        out+=bytes(m)
open('re/tools/rw_20002210.bin','wb').write(out[:size])
print(hex(src),hex(dst),hex(size),hex(i),len(out))
