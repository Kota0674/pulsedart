# last 16 gauge AverageCurrent samples (5 s apart), newest last (addresses: build 0.4.1)
init
set n [lindex [read_memory 0x20005e83 8 1] 0]
set out ""
for {set k 16} {$k > 0} {incr k -1} {
  set i [expr {($n - $k) & 0xFF}]
  if {$n - $k < 0} continue
  set b [read_memory [expr {0x20005d14 + 2 * ($i % 16)}] 8 2]
  set v [expr {[lindex $b 0] | ([lindex $b 1] << 8)}]
  if {$v > 32767} { set v [expr {$v - 65536}] }
  append out "$v "
}
echo "idx=$n mA: $out"
nrf52.dap dpreg 4 0
shutdown
