#!/usr/bin/env bash
# Program our firmware into the MOUSE app region (0x50000+) and verify.
# Only the sectors covered by the image are erased: the stock boot stub, the USB
# bootloader, the stock pairing page 0xEF000 and the UICR are left untouched.
#   usage: ./flash_mouse.sh firmware.hex
set -euo pipefail
cd "$(dirname "$0")"
HEX=${1:?usage: $0 firmware.hex}

# sanity: refuse images that touch anything below 0x50000 or the UICR
python3 - "$HEX" <<'EOF'
import sys
base = 0; lo = None; hi = 0
for l in open(sys.argv[1]):
    n, a, t = int(l[1:3], 16), int(l[3:7], 16), int(l[7:9], 16)
    if t == 2: base = int(l[9:13], 16) << 4
    if t == 4: base = int(l[9:13], 16) << 16
    if t == 0:
        A = base + a; lo = A if lo is None else min(lo, A); hi = max(hi, A + n)
print(f"image 0x{lo:05x}-0x{hi:05x}")
if lo < 0x50000 or hi > 0xEE000:
    sys.exit("REFUSING: image outside 0x50000-0xEE000")
EOF

openocd -f pulsedart_nolvl.cfg \
    -c "init" \
    -c "if {[lindex [read_memory 0x10000100 32 1] 0] != 0x52840} { echo {not the mouse}; shutdown; exit 1 }" \
    -c "if {[lindex [read_memory 0x00000000 32 1] 0] != 0x20004218} { echo {REFUSING: no stock boot stub at 0x0 (MCUboot installed?). Update over USB (dfu-util) or use flash_mcuboot.sh; restore_full_stock.sh brings the stock layout back.}; shutdown; exit 1 }" \
    -c "reset halt" \
    -c "flash write_image erase $HEX" \
    -c "verify_image $HEX" \
    -c "reset run" \
    -c "shutdown" 2>&1 | grep -vE "swdio to input"
