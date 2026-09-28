# Read the external settings EEPROM (I2C 0x50, 16-bit addressing, 64 KB) through
# the chip's own TWI1 peripheral, driven over SWD. The CPU is halted so the stock
# firmware cannot touch the bus; RAM above 0x20010000 is unused by stock fw.
# Only an address-set + sequential read is issued: nothing is written to the EEPROM.
init
halt

set TWI    0x40004000
set TXBUF  0x20010000
set RXBUF  0x20010010
set CHUNK  0x4000
set TOTAL  0x10000

proc tw {off val} { global TWI; mww [expr {$TWI + $off}] $val }
proc tr {off}     { global TWI; return [lindex [read_memory [expr {$TWI + $off}] 32 1] 0] }

# Stock uses legacy TWI; our firmware uses TWIM (for the gauge at 0x55 too). The halt can
# freeze a TWIM transfer mid-way (its ISR never sends STOP), so stop it and power-cycle
# the peripheral before use.
tw 0x014 1                ;# TASKS_STOP
sleep 5
tw 0x500 0
tw 0xffc 0                ;# peripheral POWER off/on: full reset of TWI1
tw 0xffc 1
tw 0x508 0                ;# PSEL.SCL P0.00
tw 0x50c 1                ;# PSEL.SDA P0.01
tw 0x500 6                ;# ENABLE = TWIM (EasyDMA)
tw 0x524 0x01980000       ;# 100 kHz, conservative
tw 0x588 0x50             ;# EEPROM 7-bit address
tw 0x200 0x1080           ;# SHORTS: LASTTX_STARTRX | LASTRX_STOP

set ok 1
for {set a 0} {$a < $TOTAL} {incr a $CHUNK} {
    mwb $TXBUF       [expr {($a >> 8) & 0xff}]
    mwb [expr {$TXBUF + 1}] [expr {$a & 0xff}]
    tw 0x544 $TXBUF ; tw 0x548 2
    tw 0x534 $RXBUF ; tw 0x538 $CHUNK
    tw 0x104 0 ; tw 0x124 0 ; tw 0x4c4 7   ;# clear STOPPED, ERROR, ERRORSRC
    tw 0x008 1                             ;# TASKS_STARTTX
    set t0 [clock milliseconds]
    while {[tr 0x104] == 0 && [tr 0x124] == 0} {
        if {[clock milliseconds] - $t0 > 5000} { echo "TIMEOUT at 0x[format %04x $a]"; set ok 0; break }
    }
    if {[tr 0x124]} {
        echo [format "I2C ERROR at 0x%04x ERRORSRC=0x%x" $a [tr 0x4c4]]
        tw 0x014 1 ; set ok 0
    }
    if {!$ok} { break }
    echo [format "chunk 0x%04x: %d bytes" $a [tr 0x53c]]
    dump_image [format "dump/eeprom_%04x.bin" $a] $RXBUF $CHUNK
}

tw 0x500 0
if {$ok} { echo "EEPROM READ OK" }
# restart the stock firmware cleanly (TWI config, RAM scratch, halted state)
reset run
shutdown
