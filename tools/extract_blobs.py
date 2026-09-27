#!/usr/bin/env python3
"""Generate the two firmware headers that contain data from the stock firmware.

They are NOT in the repository (PixArt SROM and HyperX tables are not ours to publish).
Build them from your own full flash dump of the mouse (see docs/flashing.md, "Dump"):

    python tools/extract_blobs.py dump/flash.bin [output_dir]

Outputs:
    fw/mouse/src/pmw3389_srom.h  - PMW3389 SROM (flash 0x64926, 4094 bytes, SROM_ID 0x05)
    fw/mouse/src/led_tables.h    - LED gamma / breathing / colour-cycle tables

The script only accepts stock firmware 1.1.0.8 (it checks the SHA-256 of every blob).
"""
import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "fw" / "mouse" / "src"

SROM = (0x64926, 4094, "e1848d529531e0e8048925b294318441eec0e1f14b85284d643293a68ff34202")
# name, flash address, length, sha256 of the bytes
TABLES = [
    ("led_gamma", 0x66996, 256, None),
    ("led_breath", 0x66b52, 188, None),
    ("led_cycle_colours", 0x66c1a, 27, None),
]
TABLES_SHA = "acb64b50c595e908521f1c5af9a75dbcf1282b34abc5a0c0c71fe3ce8bfd9aec"


def sha(b):
    return hashlib.sha256(b).hexdigest()


def c_rows(b, cols, indent):
    return "\n".join(indent + ", ".join(f"0x{x:02x}" for x in b[i:i + cols]) + ","
                     for i in range(0, len(b), cols))


def srom_header(d):
    addr, n, want = SROM
    b = d[addr:addr + n]
    if sha(b) != want:
        sys.exit(f"SROM at 0x{addr:05x} does not match stock 1.1.0.8 (sha256 {sha(b)})")
    body = c_rows(b, 12, "\t")
    body = body[:-1]  # the original has no trailing comma after the last byte
    return (
        "/*\n"
        " * PMW3389 SROM firmware, extracted from the stock Pulsefire Dart firmware 1.1.0.8\n"
        f" * (flash 0x{addr:05x}, {n} bytes, SROM_ID 0x05). See re/sensor.md section 4.\n"
        f" * sha256 {want}\n"
        " */\n"
        "#pragma once\n"
        "#include <stdint.h>\n\n"
        "#define PMW3389_SROM_ID 0x05\n\n"
        f"static const uint8_t pmw3389_srom[{n}] = {{\n{body}\n}};\n"
    )


def led_header(d):
    parts = ["/* LED tables extracted from the stock firmware 1.1.0.8 (see re/led_effects.md). */",
             "#pragma once", "#include <stdint.h>", ""]
    for name, addr, n, _ in TABLES:
        b = d[addr:addr + n]
        if name == "led_cycle_colours":
            parts.append(f"/* source: flash 0x{addr:05x}..0x{addr + n - 1:05x} */")
            parts.append(f"static const uint8_t {name}[9][3] = {{")
            parts += ["    { " + ", ".join(f"0x{x:02x}" for x in b[i:i + 3]) + " },"
                      for i in range(0, n, 3)]
        else:
            parts.append(f"/* source: flash 0x{addr:05x}..0x{addr + n - 1:05x} ({n} bytes) */")
            parts.append(f"static const uint8_t {name}[{n}] = {{")
            parts.append(c_rows(b, 16, "    "))
        parts += ["};", ""]
    return "\n".join(parts)


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    out = Path(sys.argv[2]) if len(sys.argv) == 3 else OUT
    d = Path(sys.argv[1]).read_bytes()
    if len(d) != 1024 * 1024:
        sys.exit("expected a full 1 MB flash dump of the nRF52840")
    tbl = b"".join(d[a:a + n] for _, a, n, _ in TABLES)
    if sha(tbl) != TABLES_SHA:
        sys.exit(f"LED tables do not match stock 1.1.0.8 (sha256 {sha(tbl)})")
    out.mkdir(parents=True, exist_ok=True)
    (out / "pmw3389_srom.h").write_text(srom_header(d), newline="\n")
    (out / "led_tables.h").write_text(led_header(d), newline="\n")
    print("written:", out / "pmw3389_srom.h", out / "led_tables.h")


if __name__ == "__main__":
    main()
