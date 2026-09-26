"""Dongle Region$$Table decoding (reuses re/tools/scatter.py decompress/classify on dongle image).
usage: python re/tools/dongle/dscatter.py -> re/tools/dongle/ram_app.bin, ram_stub.bin (base 0x20000000)"""
import sys, struct
sys.argv=['x']
import importlib.util
spec=importlib.util.spec_from_file_location('sc','re/tools/scatter.py')
src=open('re/tools/scatter.py').read().split("if __name__ == '__main__':")[0]
src=src.replace("open('dump/flash.bin', 'rb')","open('dump/dongle/flash.bin', 'rb')").replace('RAM_SIZE = 0x40000','RAM_SIZE = 0x6000')
g={}; exec(src,g)
def rle(src,n):
    # armlink __decompress variant at 0x5364 (zero-run RLE): b=token; lit=b&15 (0->next); run=b>>4 (0->next);
    # copy lit-1 literal bytes, then emit run-1 zero bytes; until n bytes out.
    img=g['img']; out=bytearray(); p=src
    while len(out)<n:
        b=img[p]; p+=1
        lit=b&15
        if lit==0: lit=img[p]; p+=1
        run=b>>4
        if run==0: run=img[p]; p+=1
        out+=img[p:p+lit-1]; p+=lit-1
        out+=bytes(1)*(run-1)
    return bytes(out[:n])
def run(name,tb,te,out):
    img=g['img']; ram=bytearray(0x6000); top=0
    print('==',name)
    for a in range(tb,te,16):
        load,exe,size,h=struct.unpack_from('<4I',img,a)
        k=g['classify'](h)
        if k=='decompress': d,_=g['decompress'](load,size); d=d[:size]
        elif k=='copy': d=img[load:load+size]
        elif k=='zeroinit': d=b'\0'*size
        elif h&~1==0x5364: d=rle(load,size)
        else: d=b''
        k = 'rle' if h&~1==0x5364 else k
        print(f'  load=0x{load:05x} exec=0x{exe:08x} size=0x{size:x} h=0x{h:x} {k}')
        ram[exe-0x20000000:exe-0x20000000+len(d)]=d; top=max(top,exe-0x20000000+size)
    open(out,'wb').write(bytes(ram[:top]))
run('stub',0x1318,0x1338,'re/tools/dongle/ram_stub.bin')
run('app',0xa958,0xa978,'re/tools/dongle/ram_app.bin')
