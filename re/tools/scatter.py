"""Decode ARMCC/armlink Region$$Table entries and build initialized RAM images.

usage: python re/tools/scatter.py
Writes re/ram_init.bin (app, base 0x20000000, covers 0x20000000..0x2000A510)
and re/tools/out/ram_init_bootloader.bin (bootloader image, same base) and
re/tools/out/ram_init_stub.bin (boot stub).

Region$$Table entry = {load_addr, exec_addr, size, handler}; handler is one of
  __scatterload_copy / __scatterload_zeroinit / __decompress (LZ-style, armlink "RW compression").
The decompressor below is a transcription of the Thumb code at 0x50520 (app),
0x2848c (bootloader) -- both byte-identical.
"""
import struct, os

img = open('dump/flash.bin', 'rb').read()
RAM_BASE = 0x20000000
RAM_SIZE = 0x40000  # nRF52840 256 KB


def u32(a): return struct.unpack_from('<I', img, a)[0]


def decompress(src, n):
    """armlink __decompress (a.k.a. __scatterload_decompress, LZ77 variant).
    token byte b:
      lit  = b & 7   (0 -> next byte);  copies lit-1 literal bytes
      run  = b >> 4  (0 -> next byte)
      if b & 8: off = next byte; copy run+2 bytes from out[-off]   (back-reference, may overlap)
      else:     emit run zero bytes
    until n output bytes produced. Returns (bytes, bytes_consumed)."""
    out = bytearray(); p = src
    while len(out) < n:
        b = img[p]; p += 1
        lit = b & 7
        if lit == 0: lit = img[p]; p += 1
        run = b >> 4
        if run == 0: run = img[p]; p += 1
        for _ in range(lit - 1):
            out.append(img[p]); p += 1
        if b & 8:
            off = img[p]; p += 1
            for _ in range(run + 2):
                out.append(out[-off])
        else:
            out.extend(b'\0' * run)
    return bytes(out), p - src


def classify(handler):
    """Identify the handler by its code bytes."""
    h = handler & ~1
    code = img[h:h + 16]
    if code.startswith(bytes.fromhex('70b58c1810f8015b')):
        return 'decompress'
    if code.startswith(bytes.fromhex('002001e001c1121f')):
        return 'zeroinit'
    if code.startswith(bytes.fromhex('02e008c8121f08c1')) or code.startswith(bytes.fromhex('103a')):
        return 'copy'
    return 'unknown'


def run(name, table_ptr_addr, out_path):
    tb, te = u32(table_ptr_addr), u32(table_ptr_addr + 4)
    ram = bytearray(RAM_SIZE)
    touched = []
    print(f'== {name}: Region$$Table 0x{tb:05x}..0x{te:05x}')
    for a in range(tb, te, 16):
        load, exe, size, h = struct.unpack_from('<4I', img, a)
        kind = classify(h)
        if kind == 'decompress':
            data, used = decompress(load, size)
            data = data[:size]
            extra = f' compressed {used} bytes (0x{load:05x}..0x{load+used:05x})'
        elif kind == 'copy':
            data = img[load:load + size]; extra = ''
        elif kind == 'zeroinit':
            data = b'\0' * size; extra = ''
        else:
            data = b''; extra = ' ??'
        print(f'  entry@0x{a:05x}: load=0x{load:05x} exec=0x{exe:08x} size=0x{size:x} handler=0x{h:05x} ({kind}){extra}')
        o = exe - RAM_BASE
        ram[o:o + len(data)] = data
        touched.append((exe, exe + size))
    top = max(e for s, e in touched)
    os.makedirs(os.path.dirname(out_path) or '.', exist_ok=True)
    open(out_path, 'wb').write(bytes(ram[:top - RAM_BASE]))
    print(f'  wrote {out_path}: base 0x{RAM_BASE:08x}, {top-RAM_BASE:#x} bytes (to 0x{top:08x})')
    return ram


if __name__ == '__main__':
    run('boot stub', 0x28c, 're/tools/out/ram_init_stub.bin')
    run('bootloader 0x28000', 0x28484, 're/tools/out/ram_init_bootloader.bin')
    run('app 0x50000', 0x50518, 're/ram_init.bin')
