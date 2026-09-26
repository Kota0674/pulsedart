# Read-only snapshot of stock-firmware state (addresses from re/*.md).
# Optional: -c "set TAG battery" to label the output; full RAM dump goes to dump/ram_$TAG.bin
if {![info exists TAG]} { set TAG snap }
init

proc w32 {name a} { echo [format "%-22s @0x%08x = 0x%08x" $name $a [lindex [read_memory $a 32 1] 0]] }
proc bytes {name a n} {
    set s ""
    foreach b [read_memory $a 8 $n] { append s [format "%02x " $b] }
    echo [format "%-22s @0x%08x : %s" $name $a $s]
}

echo "=== $TAG ==="
w32 RESETREAS     0x40000400
w32 WDT.RUNSTATUS 0x40010400
w32 WDT.REQSTATUS 0x40010404
w32 WDT.CRV       0x40010504
w32 USBREGSTATUS  0x40000438
w32 GPIO.P0.IN    0x50000510
w32 GPIO.P1.IN    0x50000810
w32 GPIO.P0.OUT   0x50000504
w32 GPIO.P1.OUT   0x50000804
bytes power_state     0x200024fb 2
bytes soc             0x2000226c 1
bytes mV              0x20002264 2
bytes battery_block   0x200076df 9
bytes activity_state  0x200022f9 1
bytes nomotion_timer  0x20002314 2
bytes link_state      0x20002330 1
bytes rf_channel      0x20002310 1
bytes pairing_record  0x20002232 8
bytes cfg_block       0x200076c4 27
bytes dpi_profile     0x20007702 38
bytes button_map      0x20007728 24
bytes sensor_state    0x200024a0 1
bytes gauge_flags     0x20002288 8

dump_image dump/ram_$TAG.bin 0x20000000 0x40000
echo "RAM saved to dump/ram_$TAG.bin"
shutdown
