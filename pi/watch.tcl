# Step 2c: watch GPIO inputs and peripheral enables, print every change (read-only).
# Duration in seconds via: -c "set DURATION 240" before -f watch.tcl
if {![info exists DURATION]} { set DURATION 240 }
init

set EN_REGS {
    SPIM0 0x40003500  SPIM1 0x40004500  SPIM2 0x40023500  SPIM3 0x4002F500
    SAADC 0x40007500  QDEC 0x40012500   PWM0 0x4001C500   PWM1 0x40021500
    USBREG 0x40000438
}
proc r32 {a} { return [lindex [read_memory $a 32 1] 0] }
set fails 0

set t0 [clock milliseconds]
set prev {}
set seen {}
echo "WATCH START"
while {[clock milliseconds] - $t0 < $DURATION * 1000} {
    # chip may drop into System OFF / reset between polls: never abort, just log
    if {[catch {
        set cur [list [r32 0x50000510] [r32 0x50000810]]
        foreach {n a} $EN_REGS { lappend cur [r32 $a] }
    }]} {
        incr fails
        if {$fails == 1} {
            echo [format "T %7.2f LINK LOST" [expr {([clock milliseconds] - $t0) / 1000.0}]]
        }
        catch {nrf52.dap dpreg 0x04 0x50000000}
        sleep 100
        set prev {}
        continue
    }
    if {$fails} {
        echo [format "T %7.2f LINK BACK after %d fails" [expr {([clock milliseconds] - $t0) / 1000.0}] $fails]
        set fails 0
    }
    if {$cur ne $prev} {
        set t [expr {([clock milliseconds] - $t0) / 1000.0}]
        set s [format "T %7.2f P0=0x%08x P1=0x%08x" $t [lindex $cur 0] [lindex $cur 1]]
        set i 2
        foreach {n a} $EN_REGS {
            set v [lindex $cur $i]
            if {$v} { append s [format " %s=%d" $n $v] }
            # first time a serial block is enabled: grab its PSEL registers
            if {$v && [lsearch $seen "$n=$v"] < 0 && [string match SPIM* $n]} {
                lappend seen "$n=$v"
                set b [expr {$a - 0x500}]
                catch {echo [format "PSEL %s EN=%d %s" $n $v [read_memory [expr {$b + 0x508}] 32 4]]}
            }
            if {$v && [lsearch $seen $n] < 0 && $n eq "QDEC"} {
                lappend seen $n
                catch {echo [format "PSEL QDEC %s" [read_memory 0x4001251C 32 3]]}
            }
            if {$v && [lsearch $seen $n] < 0 && $n eq "SAADC"} {
                lappend seen $n
                catch {echo [format "PSEL SAADC %s" [read_memory 0x40007510 32 16]]}
            }
            incr i
        }
        echo $s
        set prev $cur
    }
}
echo "WATCH END"
shutdown
