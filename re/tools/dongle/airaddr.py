"""Compute nRF52 RADIO BASE/PREFIX register values and the on-air address for a Primax ESB
pairing record, exactly as nrf_esb update_radio_addresses does it on both mouse and dongle
(addr_conv = RBIT(word) [dongle 0x7828, mouse 0x54c60]; bytewise_bitswap = REV(RBIT(word)) [dongle 0x7ab4, mouse 0x565a0]).
usage: python re/tools/dongle/airaddr.py 4a 91 3c d7 [prefix=02]"""
import sys, struct
def rbit32(x): return int(f'{x:032b}'[::-1], 2)
def rev32(x): return struct.unpack('<I', struct.pack('>I', x))[0]
def addr_conv(b): return rbit32(struct.unpack('<I', bytes(b))[0])
def bytewise_bitswap(b): return rev32(rbit32(struct.unpack('<I', bytes(b))[0]))
def air(base_reg, prefix_byte_reg, balen=4):
    # nRF52: address sent base (bits 0..8*balen-1 of BASE, low byte first) then prefix; each byte LSbit first
    bits = []
    for i in range(balen):
        byte = (base_reg >> (8 * i)) & 0xff
        bits += [(byte >> k) & 1 for k in range(8)]
    bits += [(prefix_byte_reg >> k) & 1 for k in range(8)]
    return bytes(int(''.join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))
rec = [int(x, 16) for x in sys.argv[1:5]]
pfx = int(sys.argv[5], 16) if len(sys.argv) > 5 else 0x02
b1 = addr_conv(rec)
prefixes = [0x01, 0xC2, pfx, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8]
p0 = bytewise_bitswap(prefixes[0:4]); p1 = bytewise_bitswap(prefixes[4:8])
print(f'record {" ".join(f"{x:02x}" for x in rec)} prefix {pfx:02x}')
print(f'BASE0=0x{addr_conv([0xEE]*4):08X} BASE1=0x{b1:08X} PREFIX0=0x{p0:08X} PREFIX1=0x{p1:08X}')
print('on-air pipe2 address (MSbit-first bytes, in transmit order):', air(b1, (p0 >> 16) & 0xff).hex(' '))
print('on-air pipe0 address:', air(addr_conv([0xEE]*4), p0 & 0xff).hex(' '))
