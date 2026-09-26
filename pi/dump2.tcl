# Step 2: read-only dump of flash, UICR and FICR. Target keeps running.
init

set t0 [clock milliseconds]
dump_image dump2/flash.bin 0x00000000 0x100000
echo [format "flash: %.1f s" [expr {([clock milliseconds]-$t0)/1000.0}]]
dump_image dump2/uicr.bin  0x10001000 0x1000
dump_image dump2/ficr.bin  0x10000000 0x1000

shutdown
