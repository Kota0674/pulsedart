# Pulsedart firmware: debug hooks (SWD only)

Service hooks in our firmware (`fw/mouse/src`). They are read/written over SWD from the Pi
(OpenOCD `read_memory` / `write_memory`, no CPU halt). Nothing reaches them from USB, BLE or the
dongle. **Addresses change with every build:** take them from
`arm-zephyr-eabi-nm build/mouse/zephyr/zephyr.elf`.

After every OpenOCD session run `nrf52.dap dpreg 4 0` (clears CDBGPWRUPREQ). Otherwise the chip
stays in debug interface mode, which raises the current and distorts measurements.

## `test_cmd` (u8, main.c)

Processed in the main loop, **also while asleep** (the 1 s sleep tick), then reset to 0.

| Value | Action |
|---|---|
| 1 | Synthetic motion. Every tick sends +1/-1 px alternating in X over the active transport. The sensor is forced to run mode (Config2 = 0x00), the LEDs are off, and the mouse stays awake. Used to measure transport current without anyone moving the mouse. |
| 2 | Stop synthetic motion (Config2 back to 0x20 = rest enabled, the post-SROM default). |
| 0x10 | Save mode ESB (dongle) and reboot. |
| 0x11 | Save mode BLE and reboot. |

## `power_avg_ma[16]` / `power_avg_idx` (power.c)

This is a ring of the bq27421 `AverageCurrent` (reg 0x10, signed mA, negative = discharge). It
takes one sample per `power_update()`, which runs every 5 s while awake and asleep. The idx is
a free-running u8.

Limit: the gauge reports **0 below about 5 mA** (deadband and gauge sleep). Sleep currents are
therefore invisible; keep a known load on (e.g. the LEDs) to see differences.

Pi scripts: `~/pulsedart/curread.tcl` (dump the ring; addresses inside must be updated per build).

## BLE link check (ble.c)

A connection that is not encrypted, or whose host has not subscribed to the mouse (or boot mouse)
report, is dropped `LINK_CHECK_MS` (15 s) after connecting. Advertising then resumes. This clears
the "connected to nobody" state seen on 2026-09-27: `cur_conn` was set and advertising was off
while neither the Pi nor Windows had a link. The log now prints the peer address on every
connection (`BLE connected: <addr>`).

## Sleep (main.c)

About 10.5 s without activity: the radio is off (ESB), the LEDs are off, and nothing is polled.
The tick is 1 s (watchdog 3 s, power poll every 5 s). Level interrupts on MOTION, the 6 buttons
and both wheel contacts wake the loop, like stock 0x56d28. `test_cmd` is still handled.
