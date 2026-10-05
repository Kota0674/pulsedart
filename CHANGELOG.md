# Changelog

Dates are when the work was done. Tags start at `v0.4.1`; the older entries describe how the
project got there.

## v0.4.2 (2026-10-05)

- **Releases without a compiler:** `fw\build-release.ps1` builds both variants with marker
  bytes instead of the sensor SROM and the LED tables, and `tools/make_firmware.py` puts that
  data in from your own dump. For MCUboot it also creates your signing key, puts the public key
  into the bootloader and signs the image. The result is byte-identical to a normal build
  except for the build time.
- The stock-derived data moved from two headers into `fw/mouse/src/stock_blobs.c`
  (`tools/extract_blobs.py` writes it), so that each array is one block in the image.
- README: Raspberry Pi wiring on the front page; settings and dongle pairing carry over.
- `pi/gauge_read.tcl`: reads the fuel gauge over SWD (standard commands only).
- A second Dart flashed: stock 1.1.0.8 again, identical except for the pairing record.

## v0.4.1 (2026-09-28)

- **Fuel gauge setup as stock:** the bq27421 check at boot, and re-programming of the stock
  data (800 mAh, 3.0 V, Ra table) when its "configured" marker is missing. On the author's
  mouse the marker was missing: the gauge was programmed once, and the next boot found it
  configured.
- NGENUITY `51 01 1` (gauge Control sub-command) implemented; `51 00` reports the gauge status.
- Fix: NGENUITY `D0 01` rejected zone 1 (the custom-LED count of the second zone); stock
  accepts zones 0 and 1.
- `pi/eeprom_restore.sh`: writes a saved settings EEPROM back over SWD. `eeprom.tcl` resets
  the TWIM first (our firmware uses TWIM, and the halt could freeze a transfer).
- BLE current difference explained: the higher numbers were taken with the LEDs on.

## v0.4.0 (2026-09-28)

- **MCUboot + firmware update over USB** (DFU, `pi/dfu_update.sh`): signed images, a
  corrupted image is rejected. It is installed on the author's mouse, so SWD wires are no
  longer needed for updates.
- NGENUITY's stock "firmware update" command is refused in the MCUboot variant (it would
  overwrite slot 0).
- MCUboot recovery: hold Left+Right+Back while switching on → USB DFU in the bootloader, blue
  LEDs (`fw/mcuboot_hooks`). MCUboot partition grown to 64 KB (slots start at 0x10000).
- Debug build (`build-mcuboot.ps1 -Debug`) with the log on a USB virtual COM port.
- Fixes: a race in the tunnel reply retry, the BLE advertising budget after our own link
  drop, and a Qi pad no longer keeps the mouse awake.
- `flash_mouse.sh` refuses to run on an MCUboot layout; `flash_mcuboot.sh` erases both slots
  first.

## v0.3.0 (2026-09-27, evening)

NGENUITY over the dongle, stock-like sleep, Bluetooth power and robustness.

### Added
- **Async `FF 03` status** to the host (USB vendor report / dongle 0xE0 packet) as stock:
  boot, wake, button edges, power changes, factory reset. The dongle-connected NGENUITY needs it
  to show the mouse as online. It is also sent on link-up and repeated every 3 s until the host's
  first command (not in stock).
- Resend of lost NGENUITY tunnel replies (up to 50 times; stock sends once).
- **BLE:**
  - connection latency 30 at 7.5 ms (no added input lag);
  - advertising budget (fast 30 s, slow up to 3 min, then off until the mouse is touched);
  - link check that drops unencrypted or unsubscribed links after 15 s;
  - the peer address in the connect log.
- SWD debug hooks (`test_cmd`: synthetic motion, mode switch) and a fuel-gauge current ring
  (`re/fw_debug.md`).
- Docs: `docs/`, `README.md`, own wiring diagrams (`tools/gen_wiring.py`).
- `tools/extract_blobs.py`: the SROM and LED tables are now generated from your own dump
  and are not part of the repository.
- `pi/dump_stock.sh`: full stock backup, read twice and compared, plus checksums; the restore
  scripts verify against the recorded checksum.
- `fw/mouse/local.conf` for personal Kconfig (the dongle record moved there).

### Changed
- **Sleep is now like stock:**
  - after about 10.5 s idle the ESB radio and LEDs go off and nothing is polled;
  - level interrupts on MOTION, the buttons and the wheel wake the mouse (2–7 ms to the first
    packet at the dongle);
  - a 1 s tick keeps the watchdog and power checks.
- A Qi pad no longer keeps the mouse awake (a cable still does, as stock).

### Removed
- System OFF after 15 min: waking from it rebooted the mouse and took about 1 s. Measurements
  showed it saved only microamps, because the sensor stays powered anyway.

### Tried and rejected (see `docs/testing.md`)
- Keeping the dongle link alive while asleep:
  - sparse pings every 0.5 s lose the link, because the dongle hops channels after 8 ms of
    silence;
  - a 4 ms keep-alive costs about 2–3 mA.

## v0.2.0: full stock feature parity (2026-09-27, night)

- Stock EEPROM settings model; lighting engine (effects, indications); DPI stages and sniper;
  button map with keyboard and consumer keys; macros (stock bugs fixed).
- Full NGENUITY protocol over USB, byte-compatible with stock 1.1.0.8.
- USB with the stock identity and 4 HID interfaces.
- ESB link to the stock dongle: keyboard/consumer packets, vendor tunnel, lost-report resend,
  channel following, pairing.
- BLE HID (mouse, keyboard, consumer) with the battery service.
- Combos: pairing, factory reset, mode switch, BLE unpair; the 3-blink mode indication.
- Battery (bq27421), charging indication, idle LEDs off.
- Code reviewed against the stock firmware notes before the first flash.

## v0.1.0: reverse engineering (2026-09-26/27)

- SWD access to the 2.1 V nRF52840 with a Raspberry Pi (no level shifter).
- Verified dumps of flash, UICR, FICR and EEPROM; the dongle (nRF52810) dumped as well.
- Notes on the stock mouse firmware: boot layout, sensor, input, USB, LEDs, power, EEPROM,
  vendor protocol, radio. Notes on the dongle firmware and the full 2.4 GHz protocol.
- Board definition and the stage-1 bring-up app.
