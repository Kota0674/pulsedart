"""Reference model of the stock vendor-command handler 0x51c44 (read side + write echo/error),
USB source (source=1). Builds post-boot RAM from dump/eeprom.bin + dump/flash.bin and prints
example request -> 64-byte reply vectors used in re/vendor_protocol.md.
usage (from the repository root): python re/tools/vendor_model.py
"""
import struct
E = open('dump/eeprom.bin', 'rb').read()
F = open('dump/flash.bin', 'rb').read()

cfg = bytearray(E[0x290:0x290 + 0x1b])
cfg[0x0f] = 5; cfg[0x10] = 0x12                      # 0x5a276
cfg[0x11:0x15] = struct.pack('<I', 10500)            # 0x5a288
cfg[0x15:0x19] = struct.pack('<I', 60000)            # 0x5a280
misc = bytearray(E[0x60:0x69])
led = bytearray(E[0x110:0x12a])
dpi = bytearray(E[0x90:0xb6])
btn = bytearray(E[0x190:0x1a8])
gauge_cnt = struct.pack('<I', E[0xfe01])             # 0x20002288 (H: boot gauge logic may change it)
pair_id = F[0xef000:0xef007]


def run(req):
    r = bytearray(req) + bytearray(64 - len(req))
    err = None
    c, s, i = r[0], r[1], r[2]
    if c == 0x50 and s == 0 and i == 0:
        r[3] = 0x1d; r[4:12] = bytes([0xe2, 0x16, 0x51, 0x09, 0x08, 0x00, 0x01, 0x01])
        name = b'HyperX Pulsefire Dart\0'; r[12:12 + len(name)] = name
    elif c == 0x50 and s == 0 and i == 1:
        r[4:12] = bytes([2, 0x0e, 1, 5, 8, 0, 1, 1])
    elif c == 0x50 and s == 3:
        r[3] = 0x31; r[4] = led[0]; r[5] = led[13]; r[6] = dpi[2]; r[7:9] = dpi[8:10]
        for k in range(5):
            r[9 + 2 * k:11 + 2 * k] = dpi[0xa + 2 * k:0xc + 2 * k]
            r[0x13 + 3 * k:0x16 + 3 * k] = dpi[0x17 + 3 * k:0x1a + 3 * k]
        for k in range(6):
            r[0x22 + 3 * k:0x25 + 3 * k] = btn[3 * k:3 * k + 3]
        r[0x34] = cfg[0x0e]
    elif c == 0x51 and s == 0 and i == 0:
        r[3] = 3; r[4:7] = misc[0:3]; r[7:9] = misc[6:8]; r[9] = misc[8]; r[10:14] = gauge_cnt
    elif c == 0x52 and s == 0 and i == 0:
        r[3] = 0x11
        for z in range(2):
            b = 4 + 10 * z; zz = led[13 * z:13 * z + 13]
            r[b] = zz[0]; r[b + 1] = zz[1]; r[b + 2] = zz[2]; r[b + 3] = zz[6]; r[b + 4:b + 10] = zz[7:13]
    elif c == 0x53 and s == 0 and i == 0:
        r[3] = 0x21; r[4] = dpi[0]; r[5] = dpi[7]; r[6:8] = dpi[3:5]; r[8:10] = dpi[5:7]; r[10:12] = dpi[8:10]
        for k in range(5):
            r[12 + 2 * k:14 + 2 * k] = dpi[0xa + 2 * k:0xc + 2 * k]
            r[22 + 3 * k:25 + 3 * k] = dpi[0x17 + 3 * k:0x1a + 3 * k]
    elif c == 0x54 and s == 0 and i == 0:
        r[3] = 0x0c
        for k in range(6):
            r[4 + 3 * k:7 + 3 * k] = btn[0:3]            # stock bug: always entry 0
    elif c == 0x57 and s == 0x13 and i == 1 and r[3] == 0:
        r[3] = 8; r[4:11] = pair_id
    elif c == 0x07:
        err = 0
    if err is not None:
        o = bytearray(64); o[0:3] = r[0:3]; o[3] = 2; o[4] = 0xec; o[5] = err
        return o
    return r


def hx(b):
    b = bytes(b)
    last = len(b)
    while last > 0 and b[last - 1] == 0: last -= 1
    return b[:max(last, 6)].hex(' ') + (f'  (+{64 - max(last, 6)} x 00)' if last < 64 else '')


for q in ['50 00 00', '50 00 01', '50 03', '51 00 00', '52 00 00', '53 00 00', '54 00 00',
          '57 13 01 00', '07']:
    req = bytes.fromhex(q)
    print(f'{q:12} -> {hx(run(req))}')
