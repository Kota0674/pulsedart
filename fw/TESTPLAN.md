# First-run test plan

Everything builds but has not run on the mouse yet. Go through the steps in order; each one
only continues if the previous one passed. The stock firmware can be restored at any
point with `./restore_stock.sh` on the Pi.

Before starting:
- Mouse wires on the Pi: pins 18 / 20 / 22 as in `pi_wiring_both.png`.
- Dongle wires on the left row may stay connected; the mouse scripts only use GPIO24/25.
- Mouse battery charged, mouse switched on.

## 0. Health check of the mouse (read-only)
```bash
cd ~/pulsedart && ./step1.sh
```
Expect `CTRL-AP IDR 0x02880000`, `INFO.PART 0x52840`, `AAD0`. This also confirms the SWDIO line
survived the 3.3 V mistake of 2026-09-27.

## 1. Bring-up image (LEDs + buttons over RTT)
Build `fw/app`, copy `app/build/app/zephyr/zephyr.hex` to the Pi as `bringup.hex`, then:
```bash
./flash_mouse.sh bringup.hex
./rtt.sh
```
Pass if:
- RTT shows `Pulsedart bring-up …`.
- The LEDs step through zone 0 R/G/B then zone 1 R/G/B, 2 s each, and the colours match the log.
- Each button prints `BTN … down/up`. The front side button is `BTN5/FWD P1.04`.

If nothing boots: `./restore_stock.sh` and report the RTT/openocd output.

## 2. Full firmware, wired (USB)
Build `fw/mouse`, copy the hex as `fw.hex`, then `./flash_mouse.sh fw.hex` and `./rtt.sh`.
- Log shows `PMW3389 up (SROM 0x05)`. If not, send the log (sensor init is the riskiest part).
- Plug the USB cable into the PC: "Pulsedart mouse" appears, the cursor moves, all 5 buttons
  and the wheel work. Check the scroll direction; if it is reversed, set `WHEEL_INVERT 1`.
- DPI tap cycles blue / yellow / green and the speed changes.

## 3. Wireless, ESB with the spare dongle
Unplug the cable, plug the (re-assembled) spare dongle into the PC.
- The log shows `using built-in pairing record: ch 2 addr 4a 91 3c d7` and `ESB link up`.
- The cursor moves through the dongle. If it doesn't, the log lines `ESB stuck` / `dongle moved us`
  and the dongle's own behaviour tell where to look.

## 4. Wireless, BLE
Hold DPI + Forward for 3 s (the mouse reboots into BLE, blue flash).
- Pair "Pulsedart" from Windows Bluetooth settings; cursor, buttons, wheel work; battery % is shown.
- Hold DPI + Forward again to go back to ESB.

## 5. Power
- Leave the mouse untouched for 10 s: the log shows `idle`; moving it wakes it instantly.
- After 15 min untouched: `System OFF`. A click or movement must wake it (it reboots).
- Charging: plug the cable, zone LEDs show amber while charging, green when full.

## What to send back if something fails
The full `./rtt.sh` output and what you did. Nothing in these steps writes below 0x50000,
the stock pairing page or the UICR.
