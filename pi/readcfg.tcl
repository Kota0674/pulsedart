# dump live stock settings (scfg @0x2000603d in build 0.4.1 (MCUboot)) without halting
init
set b 0x2000603d
echo "poll_idx [read_memory [expr {$b+0x0e}] 8 1]  (0..3 = 125/250/500/1000)"
echo "misc     [read_memory [expr {$b+27}] 8 9]"
echo "dpi      [read_memory [expr {$b+36}] 8 38]"
echo "zone0    [read_memory [expr {$b+74}] 8 13]"
echo "zone1    [read_memory [expr {$b+87}] 8 13]"
echo "btn      [read_memory [expr {$b+100}] 8 24]"
shutdown
