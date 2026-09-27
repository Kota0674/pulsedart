# Watch the ESB (dongle) link state without halting the CPU; print every change with a
# unix-ms timestamp. Addresses are for one build (fw_rev6); after rebuilding take them from
#   arm-zephyr-eabi-nm build/mouse/zephyr/zephyr.elf
# (link_up .. esb_up are consecutive bytes: link_up, search_fails, searching, fail_count,
#  data_channel, -, -, parked, esb_up)
#   usage: openocd -f pulsedart_nolvl.cfg -f linkwatch.tcl     (runs 60 s)
init
set last ""
set t_end [expr {[clock milliseconds] + 60000}]
while {[clock milliseconds] < $t_end} {
  set a [read_memory 0x20005abc 8 9]
  set cc [read_memory 0x200011d9 8 1]
  set t [read_memory 0x20005a30 8 2]
  set s [read_memory 0x20005938 8 2]
  set cur "link=[lindex $a 0] srch=[lindex $a 2] fail=[lindex $a 3] dch=[lindex $a 4] cur=$cc park=[lindex $a 7] up=[lindex $a 8] tun_tx/rx=$t status_pending/countdown=$s"
  if {$cur ne $last} { echo "[clock milliseconds] $cur"; set last $cur }
}
nrf52.dap dpreg 4 0
shutdown
