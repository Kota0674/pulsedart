# Abort immediately if the chip on the wires is the mouse (nRF52840), not the dongle (nRF52810).
init
if {[catch {set part [lindex [read_memory 0x10000100 32 1] 0]}]} { set part 0 }
if {$part == 0x52840} {
    echo "!! MOUSE (nRF52840) is connected, not the dongle. Disconnect mouse wires. Aborting."
    shutdown
    exit 1
}
echo [format "target INFO.PART = 0x%05x" $part]
