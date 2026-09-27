# wake latency: parked 1->0 (wake detected) to link_up 0->1 (first ACKed packet). fw_wake
init
set lastp -1; set lastl -1; set t_end [expr {[clock milliseconds] + 90000}]
while {[clock milliseconds] < $t_end} {
  set a [read_memory 0x20005abc 8 8]
  set l [lindex $a 0]; set p [lindex $a 7]
  if {$p != $lastp || $l != $lastl} { echo "[clock milliseconds] link=$l parked=$p"; set lastp $p; set lastl $l }
}
shutdown
