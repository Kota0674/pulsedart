"""Emulate Keil __scatterload table at 0x66f24..0x66f44 and produce RAM image re/tools/ram_init.bin (base 0x20000000)."""
import struct
img=open('dump/flash.bin','rb').read()
ram=bytearray(0x40000)
def decompress(src,dst,n):
    end=dst+n
    while dst<end:
        b=img[src]; src+=1
        c=b&7
        if c==0: c=img[src]; src+=1
        l=b>>4
        if l==0: l=img[src]; src+=1
        for _ in range(c-1): ram[dst-0x20000000]=img[src]; src+=1; dst+=1
        if b&8:
            off=img[src]; src+=1
            s=dst-off
            for _ in range(l+2): ram[dst-0x20000000]=ram[s-0x20000000]; s+=1; dst+=1
        else:
            for _ in range(l): ram[dst-0x20000000]=0; dst+=1
for t in range(0x66f24,0x66f44,16):
    src,dst,n,fn=struct.unpack_from('<IIII',img,t)
    print(f'entry src=0x{src:x} dst=0x{dst:x} len=0x{n:x} fn=0x{fn:x}')
    if src<len(img) and (fn&~1)==0x50520: decompress(src,dst,n)
open('re/tools/ram_init.bin','wb').write(ram)
