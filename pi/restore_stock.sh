#!/usr/bin/env bash
# Put the stock application back: rewrite 0x50000-0xFFFFF of the mouse from the
# verified dump (dump/flash.bin, checked against dump/flash.bin.sha256). Boot stub, bootloader
# and UICR are below 0x50000 / separate and are not touched.
set -euo pipefail
cd "$(dirname "$0")"
# the checksum was recorded when the dump was taken and verified (docs/flashing.md)
sha256sum -c dump/flash.bin.sha256
python3 -c "d=open('dump/flash.bin','rb').read(); open('dump/stock_50000.bin','wb').write(d[0x50000:])"

openocd -f pulsedart_nolvl.cfg \
    -c "init" \
    -c "if {[lindex [read_memory 0x10000100 32 1] 0] != 0x52840} { echo {not the mouse}; shutdown; exit 1 }" \
    -c "reset halt" \
    -c "flash write_image erase dump/stock_50000.bin 0x50000 bin" \
    -c "verify_image dump/stock_50000.bin 0x50000 bin" \
    -c "reset run" \
    -c "shutdown" 2>&1 | grep -vE "swdio to input"
