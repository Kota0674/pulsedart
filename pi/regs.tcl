# Step 2b: read-only snapshot of GPIO + peripheral pin config while stock fw runs.
# Prints "REG <addr> <value>" lines; decoded on the PC by decode_regs.py.
init

proc rd {addr n} {
    set vals [read_memory $addr 32 $n]
    for {set i 0} {$i < $n} {incr i} {
        echo [format "REG 0x%08x 0x%08x" [expr {$addr + 4*$i}] [lindex $vals $i]]
    }
}

# GPIO P0 / P1: OUT, IN, DIR, then PIN_CNF[]
rd 0x50000504 1 ; rd 0x50000510 2 ; rd 0x50000700 32
rd 0x50000804 1 ; rd 0x50000810 2 ; rd 0x50000A00 16

# Serial boxes (SPIM/SPI/TWIM/UARTE share bases): ENABLE + PSEL block
foreach base {0x40002000 0x40003000 0x40004000 0x40023000 0x4002F000 0x40028000} {
    rd [expr {$base + 0x500}] 1
    rd [expr {$base + 0x508}] 4
}
# SAADC: ENABLE, CH[0..7] PSELP/PSELN
rd 0x40007500 1
for {set c 0} {$c < 8} {incr c} { rd [expr {0x40007510 + 0x10*$c}] 2 }
# GPIOTE CONFIG[0..7]
rd 0x40006510 8
# PWM0..3: ENABLE, PSEL.OUT[0..3]
foreach base {0x4001C000 0x40021000 0x40022000 0x4002D000} {
    rd [expr {$base + 0x500}] 1 ; rd [expr {$base + 0x560}] 4
}
# QDEC: ENABLE, PSEL LED/A/B
rd 0x40012500 1 ; rd 0x4001251C 3
# POWER MAINREGSTATUS, USBREGSTATUS; USBD ENABLE
rd 0x40000640 1 ; rd 0x40000438 1 ; rd 0x40027500 1

# 20 samples of P0/P1 IN to catch toggling lines
for {set k 0} {$k < 20} {incr k} {
    set a [lindex [read_memory 0x50000510 32 1] 0]
    set b [lindex [read_memory 0x50000810 32 1] 0]
    echo [format "SAMPLE 0x%08x 0x%08x" $a $b]
    sleep 50
}

shutdown
