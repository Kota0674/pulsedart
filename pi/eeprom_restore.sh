#!/usr/bin/env bash
# Write a saved EEPROM image (64 KB, I2C 0x50) back to the mouse over SWD, then verify.
# Only the 128-byte pages that differ are written. The CPU is halted during the access
# (same method as eeprom.tcl) and the mouse is reset at the end.
#   usage: ./eeprom_restore.sh dump/eeprom_before_reset_20260928.bin
set -euo pipefail
cd "$(dirname "$0")"
IMG=${1:?usage: $0 eeprom_image.bin}
C="dump/eeprom_0000.bin dump/eeprom_4000.bin dump/eeprom_8000.bin dump/eeprom_c000.bin"

read_eeprom() {   # -> $1
    openocd -f pulsedart_nolvl.cfg -f eeprom.tcl 2>&1 | grep "EEPROM READ OK" >/dev/null || { echo "EEPROM read failed"; exit 1; }
    cat $C > "$1"; rm $C
}

read_eeprom /tmp/ee_now.bin
python3 - "$IMG" /tmp/ee_now.bin /tmp/ee_write.tcl <<'EOF'
import sys
want = open(sys.argv[1], 'rb').read(); now = open(sys.argv[2], 'rb').read()
assert len(want) == len(now) == 65536, "images must be 64 KB"
pages = [a for a in range(0, 65536, 128) if want[a:a + 128] != now[a:a + 128]]
print(f"{len(pages)} page(s) differ: " + " ".join(f"0x{a:04x}" for a in pages))
with open(sys.argv[3], 'w') as f:
    f.write('''init
halt
set TWI 0x40004000
set TXBUF 0x20010000
proc tw {off val} { global TWI; mww [expr {$TWI + $off}] $val }
proc tr {off} { global TWI; return [lindex [read_memory [expr {$TWI + $off}] 32 1] 0] }
tw 0x014 1
sleep 5
tw 0x500 0
tw 0xffc 0
tw 0xffc 1
tw 0x508 0
tw 0x50c 1
tw 0x500 6
tw 0x524 0x01980000
tw 0x588 0x50
tw 0x200 0x200
set ok 1
proc page {a data} {
    global TXBUF ok
    write_memory $TXBUF 8 [concat [list [expr {($a >> 8) & 0xff}] [expr {$a & 0xff}]] $data]
    tw 0x544 $TXBUF ; tw 0x548 130
    tw 0x104 0 ; tw 0x124 0 ; tw 0x4c4 7 ; tw 0x160 0 ; tw 0x14c 0
    tw 0x008 1
    # 130 bytes at 100 kHz take ~12 ms, the EEPROM write cycle up to 5 ms: wait, then check once
    sleep 100
    if {[tr 0x124] || [tr 0x4c4]} { echo "I2C ERROR at $a"; tw 0x014 1; set ok 0; return }
    if {![tr 0x104] || [tr 0x54c] != 130} { echo "TIMEOUT at $a"; tw 0x014 1; set ok 0; return }
}
''')
    for a in pages:
        f.write(f"if {{$ok}} {{ page {a} {{{' '.join(str(b) for b in want[a:a + 128])}}} }}\n")
    f.write('''tw 0x500 0
if {$ok} { echo "EEPROM WRITE OK" }
reset run
shutdown
''')
if not pages:
    sys.exit(3)
EOF
rc=$?
openocd -f pulsedart_nolvl.cfg -f /tmp/ee_write.tcl 2>&1 | grep -E "EEPROM WRITE OK|ERROR|TIMEOUT"
read_eeprom /tmp/ee_after.bin
cmp "$IMG" /tmp/ee_after.bin && echo "VERIFIED: EEPROM now equals $IMG"
