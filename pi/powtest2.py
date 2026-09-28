import subprocess, time, re, sys
TEST, RING, IDX = 0x20005e6f, 0x20005d14, 0x20005e83
def ocd(*cmds):
    a = ['openocd', '-f', 'pulsedart_nolvl.cfg', '-c', 'init']
    for c in cmds: a += ['-c', c]
    a += ['-c', 'nrf52.dap dpreg 4 0', '-c', 'shutdown']
    return subprocess.run(a, capture_output=True, text=True).stdout + subprocess.run(['true']).__class__.__name__
def ring():
    out = subprocess.run(['openocd', '-f', 'pulsedart_nolvl.cfg', '-c', 'init', '-c',
        f'echo "RING: [read_memory {IDX:#x} 8 1] [read_memory {RING:#x} 16 16]"', '-c', 'nrf52.dap dpreg 4 0',
        '-c', 'shutdown'], capture_output=True, text=True).stderr
    v = [int(x, 16) for x in re.search(r"RING: (.*)", out).group(1).split()]
    idx, vals = v[0], [x - 65536 if x > 32767 else x for x in v[1:]]
    return idx, vals
def cmd(c):
    subprocess.run(['openocd', '-f', 'pulsedart_nolvl.cfg', '-c', 'init', '-c', f'mwb {TEST:#x} {c}',
                    '-c', 'nrf52.dap dpreg 4 0', '-c', 'shutdown'], capture_output=True)
def phase(name, c, secs, settle=10):
    if c is not None: cmd(c)
    time.sleep(settle)
    i0, _ = ring()
    time.sleep(secs)
    i1, vals = ring()
    n = min((i1 - i0) & 0xFF, 16)
    s = [vals[(i1 - k) % 16] for k in range(n, 0, -1)]
    avg = sum(s) / len(s) if s else float('nan')
    print(f'{name:38s} samples={n:2d} avg={avg:6.1f} mA  {s}', flush=True)
for step in sys.argv[1:]:
    exec(step)
