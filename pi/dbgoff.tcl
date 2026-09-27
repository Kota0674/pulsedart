# Clear CDBGPWRUPREQ so the chip leaves debug interface mode (extra current otherwise).
init
nrf52.dap dpreg 4 0
shutdown
