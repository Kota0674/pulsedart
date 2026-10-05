# Programmer scripts (Raspberry Pi + OpenOCD)

Copy this folder to the Pi (`~/pulsedart`) and run the scripts from there. Wiring and warnings:
[../docs/hardware.md](../docs/hardware.md). Procedures: [../docs/flashing.md](../docs/flashing.md).

Scripts marked **build-specific** contain RAM addresses of one firmware build; update them from
`arm-zephyr-eabi-nm build/mouse/zephyr/zephyr.elf` after rebuilding. All of them end with
`nrf52.dap dpreg 4 0`, or `dbgoff.tcl` should be run afterwards (see
[../re/fw_debug.md](../re/fw_debug.md)).

## OpenOCD configs

| File | Target |
|---|---|
| `pulsedart_nolvl.cfg` | **the mouse** (2.1 V) without a level shifter: CLK through a 1k/2k divider, DIO open-drain through 150 Ω, 100 kHz |
| `pulsedart.cfg` | the mouse through a level shifter (first plan, not used) |
| `dongle_ok.cfg` | the dongle's nRF52810 (3.3 V): SWCLK GPIO27, SWDIO GPIO17, **working config** |
| `dongle.cfg`, `dongle_tp3.cfg` | pad permutations tried while finding the dongle's SWD pads |
| `dongle_guard.tcl` | included by the dongle configs: aborts if the chip is the mouse |

## Safe (read-only) checks and dumps

| File | What |
|---|---|
| `step1.sh` + `check_approtect.tcl` | first contact: IDCODE, FICR, APPROTECT state |
| `dump_stock.sh` | **full stock backup** (flash/UICR/FICR twice and compared, EEPROM, checksums) |
| `dump.tcl`, `dump2.tcl` | flash + UICR + FICR to `dump/` or `dump2/` (used by `dump_stock.sh`) |
| `eeprom.tcl` | settings EEPROM through the chip's TWIM (halts the CPU, resets after) |
| `gauge_read.tcl` | fuel gauge (bq27421) through the chip's TWIM: charge, capacity, health, and whether the stock data is programmed (DesignCapacity 800 mAh, ITPOR 0). Standard commands only, nothing is written |
| `eeprom_restore.sh img` | writes a saved EEPROM image back (changed pages only), verifies |
| `usbtest.py` | USB self-test: NGENUITY queries (battery, gauge, lighting, DPI) + mouse reports under synthetic motion (`test_cmd` address is build-specific) |
| `powtest2.py "step" ...` | battery current per phase from the gauge ring (e.g. `"phase(\"BLE moving\", 1, 45)"`, `"cmd(0x10)"`); USB unplugged; addresses are build-specific |
| `regs.tcl` + `decode_regs.py`, `awake.tcl`, `watch.tcl`, `live.tcl`, `radiowatch.tcl` | live observation of the **stock** firmware (GPIO, peripherals, radio state), used for the reverse engineering |
| `evread.py` | on the Pi: print the input events of the mouse connected over BLE (button and motion test) |

## Flashing and restoring

| File | What |
|---|---|
| `flash_mouse.sh fw.hex` | program our firmware at 0x50000+ (refuses anything else), verify |
| `restore_stock.sh` | stock application back from `dump/flash.bin` (0x50000–0xFFFFF) |
| `restore_full_stock.sh` | the whole 1 MB back (after the MCUboot variant) |
| `flash_mcuboot.sh mcuboot.hex app.signed.hex` | one-time switch to MCUboot over SWD (replaces the stock boot stub) |
| `dfu_update.sh zephyr.signed.bin` | **firmware update over USB** (MCUboot variant, mouse on a USB cable to this machine) |

## Working with our firmware

| File | What |
|---|---|
| `rtt.sh` | RTT log (start it before the event: the buffer is small) |
| `dbgoff.tcl` | clear the debug power request (leave debug interface mode) |
| `cmd.tcl` (build-specific) | `openocd -f pulsedart_nolvl.cfg -c "set CMD 1" -f cmd.tcl`: debug command (`test_cmd`) |
| `powtest.sh LABEL SECONDS` | synthetic motion for N s, then print the fuel-gauge current ring |
| `curread.tcl` (build-specific) | print the fuel-gauge current ring |
| `readcfg.tcl` (build-specific) | stock settings in RAM (poll rate, DPI, lighting zones, button map) |
| `ledsample.tcl` | LED PWM duties over time (lighting checks without looking at the mouse) |
| `linkwatch.tcl` (build-specific) | ESB link / channel / tunnel / status state, printed on change |
| `wakewatch.tcl` (build-specific) | sleep entry and wake → first ACK latency |
| `bst.tcl` (build-specific) | BLE link state (connection, encryption, subscription, advertising) |
