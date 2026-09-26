# Step 1: read-only probe of the nRF52840 access port state.
# Nothing here writes to the chip.
#
# CTRL-AP is AP #1:
#   0x0FC IDR              expected 0x02880000 (Nordic CTRL-AP)
#   0x00C APPROTECTSTATUS  1 = debug access open, 0 = protected

init

set idr    [nrf52.dap apreg 1 0xfc]
set status [nrf52.dap apreg 1 0x0c]

echo [format "CTRL-AP IDR:          0x%08x" $idr]
echo [format "APPROTECTSTATUS:      0x%08x" $status]

if {$idr != 0x02880000} {
    echo "!! Unexpected CTRL-AP IDR - check wiring, GND, voltage level, speed."
    shutdown
}

if {($status & 1) == 0} {
    echo "RESULT: PROTECTED - AHB-AP (flash/RAM) is not accessible."
    echo "        Do NOT run nrf52_recover yet: it mass-erases the stock firmware."
    shutdown
}

echo "RESULT: OPEN - reading identification registers."

# FICR (read-only factory info)
set part    [lindex [read_memory 0x10000100 32 1] 0]
set variant [lindex [read_memory 0x10000104 32 1] 0]
set flash   [lindex [read_memory 0x10000110 32 1] 0]
# UICR
set uicr_ap [lindex [read_memory 0x10001208 32 1] 0]

# INFO.VARIANT is ASCII, e.g. "AAD0" / "AAF0" (build code tells silicon revision)
set v [format "%c%c%c%c" [expr {($variant>>24)&0xff}] [expr {($variant>>16)&0xff}] \
                         [expr {($variant>>8)&0xff}]  [expr {$variant&0xff}]]

echo [format "FICR INFO.PART:       0x%08x" $part]
echo [format "FICR INFO.VARIANT:    0x%08x (%s)" $variant $v]
echo [format "FICR INFO.FLASH:      %d KB" $flash]
echo [format "UICR.APPROTECT:       0x%08x" $uicr_ap]

shutdown
