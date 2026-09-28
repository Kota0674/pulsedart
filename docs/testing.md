# Verification on hardware

All on the author's mouse (stock firmware 1.1.0.8 before), Windows 11 host, spare HP-era
dongle 03F0:068E, Raspberry Pi 4 as the SWD probe and second BLE host. Dates are 2026-09-27
unless noted. "SWD" means the value was read from the running firmware's RAM over SWD without
halting the CPU (`pi/readcfg.tcl` and friends); this is how most results were checked,
independently of what the host displays.

## Basics

| Item | Result | How |
|---|---|---|
| Sensor init (SROM upload, ID 0x05), motion | OK | RTT log, cursor |
| Buttons, debounce, wheel direction | OK | user test |
| USB enumeration with the stock identity, 4 interfaces | OK | Windows device list |
| ESB link to the stock dongle, dongle channel hopping | OK | cursor, RTT log |
| BLE HID pairing and reconnect (Linux BlueZ on the Pi, Windows 11) | OK | cursor, `btmon`, Windows device list |
| BLE connection parameters 7.5 ms / latency 30 accepted | OK (BlueZ) | `btmon` LE Connection Update Complete |
| Battery % and voltage | OK | RTT log, NGENUITY |
| Charging indication (white breathing) | OK | user |
| Lighting effects, DPI indication, 3-blink mode indication | OK | user, SWD PWM readout |
| Mode switch ESB ↔ BLE (combo and SWD `test_cmd`) | OK | user, SWD |
| Factory reset (M + DPI, 5 s): lighting and button map back to the stock defaults, persisted across a reboot (2026-09-28) | OK | SWD RAM readout before/after, EEPROM backup taken first |

## NGENUITY (Windows, NGENUITY 2.38)

It was controlled through UI Automation, and each change was verified in the mouse RAM over SWD.

| Item | USB | Dongle |
|---|---|---|
| Mouse detected, battery shown | OK | OK (needed the `FF 03` status) |
| Lighting: effect, speed, brightness | OK | OK (NGENUITY sends the colour cycle as a stream over USB, a single colour over the dongle) |
| Polling rate | OK | OK |
| DPI stages / values / colours | OK | OK |
| Button remap (mouse function, multimedia: Volume Up works) | OK | – |
| Macro recorded in NGENUITY ("hi"), assigned, played from the button | OK | – |
| Settings survive a reset (saved to EEPROM) | OK | – |
| Stays online while the mouse sleeps | – | no, as stock (radio off while asleep; online again about 3 s after motion) |
| Mouse still goes to sleep while NGENUITY is open | – | OK (woken, online, idle about 13 s later) |

## Sleep and wake

| Item | Result |
|---|---|
| Idle entry after about 10.5 s, radio off, LEDs off | OK (SWD `parked`, PWM duties 0) |
| Wake by motion, button, wheel (level interrupts) | OK (user) |
| Wake to first ACK at the dongle | 2–7 ms (8 wakes measured), 71 ms once when the link was mid-hop |
| Reboot-style wake (the removed System OFF variant) | 944–963 ms to first ACK, plus about 2 s of channel settling |

## Current measurements (bq27421 AverageCurrent, 1 mA resolution, 5 s samples)

The gauge reads 0 below about 5 mA (deadband / gauge sleep), so sleep currents are not
measurable with it. The synthetic-motion tests use `test_cmd = 1` (SWD, see `re/fw_debug.md`):
+1/−1 px alternating, sensor forced to run mode, LEDs off.

| Condition | Current |
|---|---|
| Moving by hand, LEDs on (static red, full), ESB | 38–39 mA |
| Moving by hand, LEDs on, BLE (Pi host) | 39–54 mA (LEDs on, like the ESB row above; uneven hand motion) |
| Synthetic motion, LEDs off, **BLE** (Windows host) | **26–27 mA** |
| Synthetic motion, LEDs off, **ESB** | **25–26 mA** |
| Awake without motion, LEDs on, ESB keep-alive | 15–16 mA |
| Asleep, radio off, LEDs forced on (as a load) | 13 mA, with the sensor's own rest mode on or explicitly re-enabled (no difference) |
| Asleep, LEDs off | < 5 mA (below the gauge's floor) |
| Re-measured with 0.4.1 (2026-09-28, on battery): BLE moving 26.3 mA, ESB moving 25.4 mA (synthetic, LEDs off); asleep in both modes < 5 mA | |

Conclusions:
- The sensor in run mode (datasheet 21 mA) dominates while moving.
- The LEDs are the next biggest consumer.
- BLE vs ESB differs by about 1 mA. The higher BLE hand-motion numbers (39–54 mA) were taken
  with the LEDs on (~13 mA) and uneven motion; with the same conditions BLE matches ESB.
- An ESB keep-alive every 4 ms would cost about 2–3 mA if kept on while asleep, which is why
  sleep turns the radio off, as stock.

## MCUboot and USB update (2026-09-28)

| Item | Result |
|---|---|
| MCUboot boots the signed image (VTOR 0xC200), BLE reconnects to the bonded Windows host | OK |
| USB DFU from the Pi (`dfu_update.sh`): 0.3.0+0 -> 0.3.0+1, version read from the slot 0 header | OK |
| Corrupted image (one byte flipped) is rejected, slot 0 unchanged, the mouse keeps working | OK |
| `flash_mouse.sh` refuses to write while MCUboot is installed | OK |
| MCUboot adds < 75 ms to a cold boot (reset to app VTOR), and nothing to wake from sleep | OK |
| Recovery: L+R+Back held at power-up, then blue LEDs, `Pulsedart recovery` on USB after 4 s, image written to slot 1, installed, clean reboot | OK |
| Debug build: log on the USB COM port (`/dev/ttyACM0`) | OK |

## Not verified yet

- Low-battery indication at a really low battery. (Qi charging verified 2026-09-28: Qi detected,
  +84…102 mA charge current, white breathing as stock.)
- Pairing with a *different* dongle through L+R+DPI (the spare dongle is used through its record).
- USB DFU from Windows (needs the WinUSB driver).
- Long-term battery life.

Older step-by-step first-run plan: `fw/TESTPLAN.md`.
