# sample LED PWM duties (no halt). PWM0 ch0-2 = wheel RGB, ch3 = logo R; PWM1 ch0-1 = logo G,B. top=5120
init
set p0 [lindex [read_memory 0x4001C520 32 1] 0]
set p1 [lindex [read_memory 0x40021520 32 1] 0]
for {set i 0} {$i < 8} {incr i} {
  set a [read_memory $p0 16 4]; set b [read_memory $p1 16 2]
  set w ""; foreach v [lrange $a 0 2] { append w [format "%4d " [expr {$v & 0x7fff}]] }
  set l [format "%4d %4d %4d" [expr {[lindex $a 3] & 0x7fff}] [expr {[lindex $b 0] & 0x7fff}] [expr {[lindex $b 1] & 0x7fff}]]
  echo "t=[format %4d [expr {$i*300}]]ms  wheel RGB $w  logo RGB $l"
  sleep 300
}
shutdown
