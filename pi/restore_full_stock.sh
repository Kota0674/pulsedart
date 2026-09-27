#!/usr/bin/env bash
# Full stock restore: rewrite the whole 1 MB flash from the verified dump
# (boot stub, HyperX USB bootloader, stock app, pairing page). UICR is not touched.
set -euo pipefail
cd "$(dirname "$0")"
# the checksum was recorded when the dump was taken and verified (docs/flashing.md)
sha256sum -c dump/flash.bin.sha256

openocd -f pulsedart_nolvl.cfg \
    -c "init" \
    -c "if {[lindex [read_memory 0x10000100 32 1] 0] != 0x52840} { echo {not the mouse}; shutdown; exit 1 }" \
    -c "reset halt" \
    -c "flash write_image erase dump/flash.bin 0x0 bin" \
    -c "verify_image dump/flash.bin 0x0 bin" \
    -c "reset run" \
    -c "shutdown" 2>&1 | grep -vE "swdio to input"
