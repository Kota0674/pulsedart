#!/usr/bin/env bash
# One-time switch to MCUboot: writes MCUboot at 0x0 (replacing the stock boot stub)
# and the signed app into slot 0 (0x10000), after erasing 0x0-0xEC000 (both slots, so no
# stale data can be taken for MCUboot trailers). Stock data 0xEE000, settings 0xF0000 and
# UICR stay untouched. Full stock restore: ./restore_full_stock.sh
#   usage: ./flash_mcuboot.sh mcuboot.hex app.signed.hex
set -euo pipefail
cd "$(dirname "$0")"
BOOT=${1:?mcuboot.hex}; APP=${2:?app.signed.hex}

python3 - "$BOOT" "$APP" <<'EOF'
import sys
def span(f):
    base = 0; lo = None; hi = 0
    for l in open(f):
        n, a, t = int(l[1:3], 16), int(l[3:7], 16), int(l[7:9], 16)
        if t == 2: base = int(l[9:13], 16) << 4
        if t == 4: base = int(l[9:13], 16) << 16
        if t == 0:
            A = base + a; lo = A if lo is None else min(lo, A); hi = max(hi, A + n)
    return lo, hi
b = span(sys.argv[1]); a = span(sys.argv[2])
print(f"mcuboot 0x{b[0]:05x}-0x{b[1]:05x}, app 0x{a[0]:05x}-0x{a[1]:05x}")
if b[0] != 0 or b[1] > 0x10000: sys.exit("REFUSING: MCUboot image outside 0x0-0x10000")
if a[0] != 0x10000 or a[1] > 0x7E000: sys.exit("REFUSING: app image outside slot 0 (0x10000-0x7E000)")
EOF

openocd -f pulsedart_nolvl.cfg \
    -c "init" \
    -c "if {[lindex [read_memory 0x10000100 32 1] 0] != 0x52840} { echo {not the mouse}; shutdown; exit 1 }" \
    -c "reset halt" \
    -c "flash erase_address 0x00000 0xEC000" \
    -c "flash write_image $BOOT" \
    -c "flash write_image $APP" \
    -c "verify_image $BOOT" \
    -c "verify_image $APP" \
    -c "reset run" \
    -c "shutdown" 2>&1 | grep -vE "swdio to input"
