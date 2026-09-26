#!/usr/bin/env bash
# Take the complete stock backup of the mouse BEFORE flashing anything (read-only).
# Dumps flash + UICR + FICR twice, checks both copies are identical, dumps the settings
# EEPROM, and records checksums that restore_stock.sh / restore_full_stock.sh verify.
#   usage: ./dump_stock.sh        (run on the Pi, mouse wired as in docs/hardware.md)
# Keep dump/ safe and private: it contains the stock firmware and your pairing record.
set -euo pipefail
cd "$(dirname "$0")"

if [ -e dump/flash.bin ]; then
    echo "dump/flash.bin already exists; move it away first (never overwrite a stock backup)."
    exit 1
fi
mkdir -p dump dump2

openocd -f pulsedart_nolvl.cfg \
    -c "init" \
    -c "if {[lindex [read_memory 0x10000100 32 1] 0] != 0x52840} { echo {not the mouse}; shutdown; exit 1 }" \
    -c "shutdown"

openocd -f pulsedart_nolvl.cfg -f dump.tcl
openocd -f pulsedart_nolvl.cfg -f dump2.tcl

for f in flash.bin uicr.bin ficr.bin; do
    cmp dump/$f dump2/$f || { echo "MISMATCH in $f: wiring is unreliable, do not flash"; exit 1; }
done
echo "flash, UICR and FICR read twice and identical"

# the EEPROM read halts the CPU, reads 64 KB in 16 KB chunks and resets the mouse after
openocd -f pulsedart_nolvl.cfg -f eeprom.tcl | grep -q "EEPROM READ OK" || { echo "EEPROM read failed"; exit 1; }
cat dump/eeprom_0000.bin dump/eeprom_4000.bin dump/eeprom_8000.bin dump/eeprom_c000.bin > dump/eeprom.bin
rm dump/eeprom_0000.bin dump/eeprom_4000.bin dump/eeprom_8000.bin dump/eeprom_c000.bin

sha256sum dump/flash.bin > dump/flash.bin.sha256
sha256sum dump/*.bin > dump/SHA256SUMS
cat dump/SHA256SUMS
echo "Stock backup complete. Copy dump/ off the Pi as well."
