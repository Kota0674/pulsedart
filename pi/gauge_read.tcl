# Read the bq27421 fuel gauge (I2C 0x55) through the chip's own TWI1, driven over SWD.
# Standard commands only: nothing is unsealed and no data memory is written.
# The CPU is halted for a few seconds and reset afterwards (as eeprom.tcl).
#   usage: openocd -f pulsedart_nolvl.cfg -f gauge_read.tcl
init
halt

set TWI    0x40004000
set TXBUF  0x20010000
set RXBUF  0x20010010

proc tw {off val} { global TWI; mww [expr {$TWI + $off}] $val }
proc tr {off}     { global TWI; return [lindex [read_memory [expr {$TWI + $off}] 32 1] 0] }

tw 0x014 1                ;# TASKS_STOP
sleep 5
tw 0x500 0
tw 0xffc 0                ;# peripheral POWER off/on: full reset of TWI1
tw 0xffc 1
tw 0x508 0                ;# PSEL.SCL P0.00
tw 0x50c 1                ;# PSEL.SDA P0.01
tw 0x500 6                ;# ENABLE = TWIM
tw 0x524 0x01980000       ;# 100 kHz
tw 0x588 0x55             ;# gauge 7-bit address

proc xfer {} {
    tw 0x104 0 ; tw 0x124 0 ; tw 0x4c4 7
    tw 0x008 1
    set t0 [clock milliseconds]
    while {[tr 0x104] == 0 && [tr 0x124] == 0} {
        if {[clock milliseconds] - $t0 > 2000} { tw 0x014 1; return -code error "timeout" }
    }
    if {[tr 0x124]} { set e [tr 0x4c4]; tw 0x014 1; return -code error [format "I2C error 0x%x" $e] }
}

# read a 16-bit little-endian standard command
proc rd16 {reg} {
    global TXBUF RXBUF
    mwb $TXBUF $reg
    tw 0x200 0x1080       ;# LASTTX_STARTRX | LASTRX_STOP
    tw 0x544 $TXBUF ; tw 0x548 1
    tw 0x534 $RXBUF ; tw 0x538 2
    xfer
    set b [read_memory $RXBUF 8 2]
    return [expr {[lindex $b 0] | ([lindex $b 1] << 8)}]
}

# Control() sub-command, then read the result
proc ctrl {sub} {
    global TXBUF
    mwb $TXBUF 0x00
    mwb [expr {$TXBUF + 1}] [expr {$sub & 0xff}]
    mwb [expr {$TXBUF + 2}] [expr {($sub >> 8) & 0xff}]
    tw 0x200 0x200        ;# LASTTX_STOP
    tw 0x544 $TXBUF ; tw 0x548 3
    tw 0x538 0
    xfer
    sleep 2
    return [rd16 0x00]
}

proc s16 {v} { return [expr {$v > 32767 ? $v - 65536 : $v}] }

if {[catch {
    echo [format "CONTROL_STATUS   0x%04x" [ctrl 0x0000]]
    echo [format "DEVICE_TYPE      0x%04x" [ctrl 0x0001]]
    echo [format "CHEM_ID          0x%04x" [ctrl 0x0008]]
    set fl [rd16 0x06]
    echo [format "Flags            0x%04x  ITPOR=%d CFGUPMODE=%d BAT_DET=%d FC=%d CHG=%d DSG=%d" $fl \
        [expr {($fl>>5)&1}] [expr {($fl>>4)&1}] [expr {($fl>>3)&1}] [expr {($fl>>9)&1}] [expr {($fl>>8)&1}] [expr {$fl&1}]]
    echo [format "Temperature      %.1f C" [expr {[rd16 0x02] / 10.0 - 273.15}]]
    echo "Voltage          [rd16 0x04] mV"
    echo "AverageCurrent   [s16 [rd16 0x10]] mA"
    echo "StateOfCharge    [rd16 0x1c] %"
    echo "RemainingCap     [rd16 0x0c] mAh"
    echo "FullChargeCap    [rd16 0x0e] mAh"
    echo "NominalAvailCap  [rd16 0x08] mAh"
    echo "FullAvailCap     [rd16 0x0a] mAh"
    echo "StateOfHealth    [expr {[rd16 0x20] & 0xff}] %"
    echo [format "OpConfig         0x%04x" [rd16 0x3a]]
    echo "DesignCapacity   [rd16 0x3c] mAh"
    echo "GAUGE READ OK"
} msg]} { echo "GAUGE READ FAILED: $msg" }

tw 0x500 0
reset run
nrf52.dap dpreg 4 0
shutdown
