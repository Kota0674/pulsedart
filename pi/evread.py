"""Read Linux input events of the mouse connected to the Pi over BLE (evdev), print
buttons and summed motion: python3 evread.py SECONDS [eventN]."""
import struct, time, sys, select
f = open('/dev/input/' + (sys.argv[2] if len(sys.argv) > 2 else 'event4'), 'rb')
FMT = 'llHHi'; SZ = struct.calcsize(FMT)
names = {(1,0x110):'LEFT',(1,0x111):'RIGHT',(1,0x112):'MIDDLE',(1,0x113):'SIDE(back)',(1,0x114):'EXTRA(fwd)'}
end = time.time() + float(sys.argv[1]); sx = sy = 0; n = 0; last = time.time()
print("listening", flush=True)
while time.time() < end:
    r, _, _ = select.select([f], [], [], 0.5)
    if not r:
        continue
    d = f.read(SZ); _, _, t, c, v = struct.unpack(FMT, d)
    if t == 2 and c in (0, 1):
        n += 1; sx += v if c == 0 else 0; sy += v if c == 1 else 0
        if time.time() - last > 1:
            print(f"motion: {n} events, sum X={sx} Y={sy}", flush=True); last = time.time()
    elif t == 2 and c in (8, 11):
        if c == 8: print(f"WHEEL {v:+d}", flush=True)
    elif t == 1:
        print(f"{names.get((t,c), hex(c))} {'down' if v else 'up'}", flush=True)
print(f"total motion events {n}, X={sx} Y={sy}")
