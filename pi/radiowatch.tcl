# Read-only: log stock radio/pairing state changes (addresses from re/radio.md)
if {![info exists DURATION]} { set DURATION 90 }
init
set vars {
    link   0x20002330  act 0x200022f9  pairflag 0x2000223d  pairstate 0x2000223b
    fails  0x20002312  ch  0x20002310  search   0x2000231b  hop       0x200022f8
}
set t0 [clock milliseconds]
set prev ""
echo "RADIOWATCH START"
while {[clock milliseconds] - $t0 < $DURATION * 1000} {
    set s ""
    if {[catch {
        foreach {n a} $vars { append s [format "%s=%d " $n [lindex [read_memory $a 8 1] 0]] }
        append s "addr=[read_memory 0x20002232 8 8]"
    }]} { set s "LINK LOST"; sleep 200 }
    if {$s ne $prev} {
        echo [format "T %6.2f %s" [expr {([clock milliseconds] - $t0) / 1000.0}] $s]
        set prev $s
    }
    sleep 20
}
echo "RADIOWATCH END"
shutdown
