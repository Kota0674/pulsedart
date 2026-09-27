# Write one debug command to test_cmd (re/fw_debug.md). BUILD-SPECIFIC address.
#   openocd -f pulsedart_nolvl.cfg -c "set CMD 1" -f cmd.tcl
#   1 synthetic motion, 2 stop, 0x10 reboot to ESB, 0x11 reboot to BLE
init
write_memory 0x20005e6f 8 $CMD
nrf52.dap dpreg 4 0
shutdown
