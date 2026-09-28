import os, sys, time, select, subprocess
def vend(cmd):
    fd = os.open('/dev/hidraw1', os.O_RDWR)
    try:
        while select.select([fd], [], [], 0)[0]: os.read(fd, 64)  # drain
        os.write(fd, bytes([0]) + bytes(cmd) + bytes(64 - len(cmd)))
        if select.select([fd], [], [], 1.0)[0]:
            return os.read(fd, 64)
    finally:
        os.close(fd)
def ocd(c):
    subprocess.run(['openocd', '-f', 'pulsedart_nolvl.cfg', '-c', 'init', '-c', c, '-c', 'shutdown'],
                   capture_output=True)
r = vend([0x51, 0, 0]);  print('51 00:', r[:14].hex(' ') if r else None)
if r: print('  soc=%d%% power_state=%d mV=%d gauge_status=%d' % (r[4], r[5], r[7] | r[8] << 8, r[9]))
r = vend([0x51, 1, 1, 0, 0x08]); print('51 01 1 (CHEM_ID):', r[:9].hex(' ') if r else None)
r = vend([0x52, 0, 0]);  print('52 00 (lighting):', r[:24].hex(' ') if r else None)
r = vend([0x53, 0, 0]);  print('53 00 (DPI):', r[:22].hex(' ') if r else None)
fd = os.open('/dev/hidraw0', os.O_RDONLY | os.O_NONBLOCK)
ocd('mwb 0x20005e6f 1')
t0 = time.time(); n = 0; first = None
while time.time() - t0 < 3:
    if select.select([fd], [], [], 0.1)[0]:
        d = os.read(fd, 64); n += 1; first = first or d
ocd('mwb 0x20005e6f 2')
os.close(fd)
print('USB mouse reports in ~3 s of synthetic motion:', n, 'first:', first.hex(' ') if first else None)
