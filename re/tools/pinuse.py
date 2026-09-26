"""List gpio helper calls with constant pin arg. usage: python re/tools/pinuse.py 9 14 43 ..."""
import sys, subprocess
pins=set(int(x) for x in sys.argv[1:])
sys.argv=['ann.py','0x50000','0x68000']
import io, contextlib
buf=io.StringIO()
with contextlib.redirect_stdout(buf):
    exec(open('re/tools/ann.py').read())
for line in buf.getvalue().splitlines():
    if '; gpio' in line:
        p=line.rsplit(' ',1)[-1]
        if p.startswith('P'):
            n=int(p[1])*32+int(p[3:])
            if n in pins: print(line)
