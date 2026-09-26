"""Decode regs.txt (output of regs.tcl) into a human-readable pin map."""
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "dump/regs.txt"
from collections import defaultdict
R, samples = defaultdict(int), []
for line in open(path):
    p = line.split()
    if p[0] == "REG":
        R[int(p[1], 16)] = int(p[2], 16)
    elif p[0] == "SAMPLE":
        samples.append((int(p[1], 16), int(p[2], 16)))

def pin(v):
    if v & 0x80000000:
        return None
    return f"P{(v >> 5) & 1}.{v & 31:02d}"

owners = {}
def claim(v, what):
    p = pin(v)
    if p:
        owners.setdefault(p, []).append(what)

SERIAL = {0x40002000: "UARTE0", 0x40003000: "SPIM0/TWIM0", 0x40004000: "SPIM1/TWIM1",
          0x40023000: "SPIM2", 0x4002F000: "SPIM3", 0x40028000: "UARTE1"}
EN = {1: "SPI", 2: "SPIS", 4: "UART", 5: "TWI", 6: "TWIM", 7: "SPIM", 8: "UARTE", 9: "TWIS"}
print("== Serial peripherals ==")
for base, name in SERIAL.items():
    en = R[base + 0x500]
    if not en:
        continue
    kind = EN.get(en, f"?{en}")
    ps = [R[base + 0x508 + 4 * i] for i in range(4)]
    if kind in ("UART", "UARTE"):
        names = ["RTS", "TXD", "CTS", "RXD"]
    elif kind in ("TWI", "TWIM", "TWIS"):
        names = ["SCL", "SDA", "-", "-"]
    else:
        names = ["SCK", "MOSI", "MISO", "CSN"]
    desc = []
    for n, v in zip(names, ps):
        if n != "-" and pin(v):
            desc.append(f"{n}={pin(v)}")
            claim(v, f"{name}:{kind} {n}")
    print(f"  {name:12} ENABLE={kind:5} " + " ".join(desc))

print("== SAADC ==")
AIN = {1: "AIN0 (P0.02)", 2: "AIN1 (P0.03)", 3: "AIN2 (P0.04)", 4: "AIN3 (P0.05)",
       5: "AIN4 (P0.28)", 6: "AIN5 (P0.29)", 7: "AIN6 (P0.30)", 8: "AIN7 (P0.31)",
       9: "VDD", 0x0D: "VDDHDIV5"}
print(f"  ENABLE={R[0x40007500]}")
for c in range(8):
    pp, pn = R[0x40007510 + 0x10 * c], R[0x40007514 + 0x10 * c]
    if pp:
        print(f"  CH{c}: P={AIN.get(pp, pp)} N={AIN.get(pn, pn) if pn else '-'}")
        if pp in range(1, 9):
            claim(0x02 + [2, 3, 4, 5, 28, 29, 30, 31][pp - 1] - 2, f"SAADC CH{c}")

print("== GPIOTE ==")
for i in range(8):
    v = R[0x40006510 + 4 * i]
    mode = v & 3
    if mode:
        pol = ["none", "LoToHi", "HiToLo", "Toggle"][(v >> 16) & 3]
        p = f"P{(v >> 13) & 1}.{(v >> 8) & 31:02d}"
        print(f"  CONFIG[{i}] {['', 'Event', '', 'Task'][mode]} {p} {pol}")
        owners.setdefault(p, []).append(f"GPIOTE{i}:{pol}")

print("== PWM / QDEC ==")
for idx, base in enumerate([0x4001C000, 0x40021000, 0x40022000, 0x4002D000]):
    if R[base + 0x500]:
        outs = [pin(R[base + 0x560 + 4 * i]) for i in range(4)]
        print(f"  PWM{idx} OUT={outs}")
        for i in range(4):
            claim(R[base + 0x560 + 4 * i], f"PWM{idx} OUT{i}")
if R[0x40012500]:
    led, a, b = (R[0x4001251C + 4 * i] for i in range(3))
    print(f"  QDEC LED={pin(led)} A={pin(a)} B={pin(b)}")
    claim(a, "QDEC A"); claim(b, "QDEC B"); claim(led, "QDEC LED")
print(f"  MAINREGSTATUS={R[0x40000640]} (1=high-voltage/VDDH)  USBREGSTATUS={R[0x40000438]:#x}  USBD.ENABLE={R[0x40027500]}")

print("== GPIO ==")
PULL = {0: "", 1: "pull-down", 3: "pull-up"}
SENSE = {0: "", 2: "sense-high", 3: "sense-low"}
tog0 = tog1 = 0
for a, b in samples:
    tog0 |= a ^ samples[0][0]
    tog1 |= b ^ samples[0][1]
for port, base, n, out, inn, tog in [(0, 0x50000700, 32, 0x50000504, 0x50000510, tog0),
                                     (1, 0x50000A00, 16, 0x50000804, 0x50000810, tog1)]:
    for i in range(n):
        c = R[base + 4 * i]
        p = f"P{port}.{i:02d}"
        d, inp, pull, drive, sense = c & 1, (c >> 1) & 1, (c >> 2) & 3, (c >> 8) & 7, (c >> 16) & 3
        own = owners.get(p, [])
        if c == 2 and not own:
            continue  # reset default: input, disconnected
        lvl = (R[inn] >> i) & 1
        parts = ["OUT" if d else "in ", "" if inp == 0 else "(buf off)", PULL.get(pull, ""),
                 SENSE.get(sense, ""), f"drive={drive}" if drive else ""]
        if d:
            parts.append(f"out={(R[out] >> i) & 1}")
        parts.append(f"lvl={lvl}")
        if (tog >> i) & 1:
            parts.append("TOGGLING")
        print(f"  {p}  cnf={c:#010x}  " + " ".join(x for x in parts if x) +
              (f"   <- {', '.join(own)}" if own else ""))
