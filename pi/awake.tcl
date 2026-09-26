# Step 2d: snapshot while the mouse is awake (read-only): PWM pin map,
# PIN_CNF, and OUT registers sampled repeatedly to catch the sensor CS line.
init
proc rd {addr n} {
    set vals [read_memory $addr 32 $n]
    for {set i 0} {$i < $n} {incr i} {
        echo [format "REG 0x%08x 0x%08x" [expr {$addr + 4*$i}] [lindex $vals $i]]
    }
}
foreach base {0x4001C000 0x40021000 0x40022000 0x4002D000} {
    rd [expr {$base + 0x500}] 1 ; rd [expr {$base + 0x560}] 4
}
rd 0x40003500 1 ; rd 0x40003508 4
rd 0x50000504 1 ; rd 0x50000510 2 ; rd 0x50000700 32
rd 0x50000804 1 ; rd 0x50000810 2 ; rd 0x50000A00 16
rd 0x40007500 1
for {set c 0} {$c < 8} {incr c} { rd [expr {0x40007510 + 0x10*$c}] 2 }
rd 0x40012500 1 ; rd 0x4001251C 3
rd 0x40006510 8
set t0 [clock milliseconds]
while {[clock milliseconds] - $t0 < 8000} {
    catch {echo [format "OUT 0x%08x 0x%08x" [lindex [read_memory 0x50000504 32 1] 0] [lindex [read_memory 0x50000804 32 1] 0]]}
}
shutdown
