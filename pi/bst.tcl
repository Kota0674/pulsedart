# BLE link state (fw_rev6 addresses)
init
echo "cur_conn=[read_memory 0x20003bf0 32 1] secured=[read_memory 0x200059e2 8 1] notif=[read_memory 0x200059df 8 1] adv=[read_memory 0x200059e1 8 1]"
nrf52.dap dpreg 4 0
shutdown
