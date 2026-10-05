# Pulsedart: open firmware for the HyperX Pulsefire Dart

A replacement firmware for the **HyperX Pulsefire Dart** wireless mouse (nRF52840 + PixArt
PMW3389), written from scratch on Zephyr / nRF Connect SDK. It keeps what the stock firmware
offers and adds Bluetooth LE.

<table>
<tr>
<td><img src="docs/img/mouse_board.jpg" alt="Main board of my Dart" width="400"></td>
<td><img src="docs/img/mouse_swd_pads.jpg" alt="SWD pads next to the nRF52840" width="400"></td>
</tr>
<tr>
<td>My Dart opened up. The SWD pads are next to the nRF52840 (U1), ground on GND2.</td>
<td>Close-up of the pads. The TX pad came off while I was soldering; it is not needed.</td>
</tr>
</table>

Why: I rarely play games and wanted to use this mouse over Bluetooth, without the dongle.
The Dart already has an nRF52840 inside, which does Bluetooth fine; the stock firmware just
never used it. So I dumped the stock firmware over SWD with a Raspberry Pi, worked out how
everything is wired and talks to the dongle and NGENUITY, and wrote my own firmware that does
everything the stock one does, plus BLE.

What works:

- **USB** wired mouse with the stock USB identity (0951:16E2, 4 HID interfaces).
- **2.4 GHz with the stock HyperX dongle** (Nordic ESB). The protocol was reverse engineered;
  the unmodified stock dongle works.
- **Bluetooth LE** HID mouse (HOGP) with battery level. The stock Dart has no Bluetooth.
- **NGENUITY** (HyperX configuration software) works over USB and over the dongle: lighting,
  DPI, polling rate, button remapping, macros, all saved to the mouse's EEPROM.
- Stock lighting engine, DPI stages, button map, macros, charging and battery indications.
- Stock-like sleep: radio and LEDs off after about 10 s idle; a motion, button or wheel
  interrupt wakes the mouse in a few milliseconds.
- Optional **MCUboot** variant: signed firmware updates over the USB cable (no more opening
  the mouse), plus a recovery mode (hold Left + Right + Back while switching on).

How often motion reaches the PC, measured on Windows 11 with
[tools/rawinterval.ps1](tools/rawinterval.ps1) while moving the mouse in circles:

| Link | Update interval | Notes |
|---|---|---|
| USB cable | **1 ms** (1000 Hz) | median gap 1.00 ms |
| Bluetooth LE | **~7.5 ms** | the BLE connection interval; each interval carries a few reports |
| Stock dongle (2.4 GHz) | **8 ms** (125 Hz) | the firmware sends every 1 ms, but reports arrive at 125 Hz through my (HP) dongle; the limit seems to be on the dongle's USB side, not investigated yet |

Waking up from sleep takes 2-7 ms until the first packet reaches the dongle. These numbers
are the report rate, not a full click-to-screen latency measurement.

> **Status:** personal project, running daily on my own mouse (mostly over Bluetooth). Tested
> on hardware, see [docs/testing.md](docs/testing.md). Not affiliated with HyperX, HP or
> Kingston. You need an SWD programmer and must open the mouse. You can brick nothing
> permanently as long as you keep your own full flash dump (the flash is not
> read-protected), but you do this **at your own risk**.

<img src="docs/img/dongle_pads.jpg" alt="Dongle pads" width="500">

The back of the stock dongle with its SWD pads (only needed if you want to look inside it).

## Wiring the Raspberry Pi as the programmer

The mouse needs only three wires, on three neighbouring pins of the **outer** header row
(the row at the board edge). Count from the end farthest from the USB ports:

| Pi pin | Signal | Goes to |
|---|---|---|
| 18 (GPIO24), 9th in the row | SWDIO | through 150 Ω to the mouse pad **DIO** |
| 20 (GND), 10th | ground | mouse **GND2**, and the lower leg of the divider |
| 22 (GPIO25), 11th | SWDCLK | through a 1 k / 2 k divider to the mouse pad **CLK** |

<img src="docs/img/pi_header_wiring.png" alt="Raspberry Pi header: mouse on the right row" width="420">

The mouse runs at **2.1 V**: never connect the Pi's 3.3 V or 5 V to it. The divider and the
open-drain SWDIO are explained in [docs/hardware.md](docs/hardware.md). `pi/step1.sh` checks the
wiring without writing anything. If it cannot connect, the clock wire is the usual suspect: I
once had it on pin 16 instead of pin 22.

## Your settings and your dongle carry over

Nothing has to be copied by hand:

- **Settings** (DPI stages, lighting, button map, macros) live in a separate EEPROM chip in
  the stock layout. Flashing does not touch it, and this firmware reads the same data.
- **The pairing with your dongle** is on its own flash page (0xEF000), which the flash scripts
  leave alone. The firmware uses it by default, so your original dongle keeps working.
- Bluetooth is new, so you pair that once in your operating system.

Checked on a second, untouched Dart: after the first flash it came up with its own DPI stages,
lighting and dongle address.

## Documentation

| Document | What is in it |
|---|---|
| [docs/hardware.md](docs/hardware.md) | Opening the mouse, SWD pads, wiring a Raspberry Pi as programmer (2.1 V target!), pin map |
| [docs/flashing.md](docs/flashing.md) | Dumping the stock firmware, building, flashing, logs, restoring stock |
| [docs/firmware.md](docs/firmware.md) | What the firmware does: modes, controls, LEDs, sleep, NGENUITY, BLE, memory map |
| [docs/testing.md](docs/testing.md) | What was verified on hardware and how; measurements |
| [docs/reverse-engineering.md](docs/reverse-engineering.md) | Index of the reverse-engineering notes in `re/` (mouse, dongle, protocols) |
| [docs/publishing.md](docs/publishing.md) | What must never go into the public repository, and why |
| [re/fw_debug.md](re/fw_debug.md) | SWD-only debug hooks (synthetic motion, mode switch, current log) |
| [CHANGELOG.md](CHANGELOG.md) | Releases and planned work |

## Repository layout

```
fw/boards/hyperx/pulsedart/   Zephyr board definition (pins, partitions)
fw/mouse/                     the firmware (src/, prj.conf, MCUboot variant in mcuboot/)
fw/mcuboot_hooks/             MCUboot hooks (recovery button combo, LEDs, watchdog)
fw/app/                       stage-1 bring-up test (LEDs + buttons over RTT)
fw/build.ps1                  build helper (Windows, nRF Connect SDK v3.4.1)
fw/build-mcuboot.ps1          same for the MCUboot variant (-Debug adds a USB log port)
pi/                           OpenOCD configs and scripts for the Raspberry Pi programmer
re/                           reverse-engineering notes of the stock mouse and dongle firmware
fw/build-release.ps1          builds a release (both variants, marker bytes instead of stock data)
tools/make_firmware.py        release + YOUR flash dump -> ready-to-flash files, no compiler
tools/extract_blobs.py        stock-derived data (sensor SROM, LED tables) from YOUR flash dump, for own builds
PINMAP.md                     nRF52840 pin map of the mouse
```

## Quick start

1. Wire an SWD programmer (above, and [docs/hardware.md](docs/hardware.md)).
2. Dump the complete stock flash, UICR and EEPROM with `pi/dump_stock.sh` and keep them safe
   ([docs/flashing.md](docs/flashing.md#1-dump-the-stock-firmware)).
3. Get the firmware files for your mouse, one of two ways:
   - **From a release, no compiler needed.** A release has marker bytes where the sensor
     firmware and the LED tables belong, because I cannot publish those. One script puts them in
     from your own dump (Python 3, plus `pip install cryptography` for the MCUboot files):
     ```
     python tools/make_firmware.py --release release/v0.4.2 --dump dump/flash.bin --out out
     ```
   - **Build it yourself** with nRF Connect SDK v3.4.1:
     `python tools/extract_blobs.py dump/flash.bin`, then `fw\build.ps1 mouse` or
     `fw\build-mcuboot.ps1`.
4. Flash, choosing one variant:
   - **Behind the stock boot code:** `pi/flash_mouse.sh out/fw.hex`. The stock boot stub and
     the HyperX USB bootloader below 0x50000 stay, and `pi/restore_stock.sh` puts the stock
     application back from your dump. Updates need the SWD wires again.
   - **MCUboot:** `pi/flash_mcuboot.sh out/mcuboot.hex out/app.signed.hex` once over SWD, then
     updates over the USB cable with `pi/dfu_update.sh out/app.signed.bin`. This replaces the
     stock boot code; the way back is `pi/restore_full_stock.sh`. `make_firmware.py` creates
     your signing key on the first run: keep it private and keep a backup. Details:
     [docs/flashing.md](docs/flashing.md#mcuboot-variant-firmware-updates-over-usb-in-use-since-2026-09-28).

## Disclaimer

This is an unofficial hobby project. It is not affiliated with, endorsed or supported by
HyperX, HP, Kingston or PixArt.

Opening the mouse, soldering to it and replacing its firmware voids your warranty and can
damage the mouse, its battery or the computer it is connected to. Everything here is provided
as is, without any warranty. You do all of this **at your own risk**; the author takes no
responsibility for any damage, data loss or other consequences. Keep a full dump of your stock
firmware before you flash anything.

## License

[MIT](LICENSE) for our own code and documentation. This does not cover:
- Zephyr (Apache-2.0) and nRF Connect SDK (LicenseRef-Nordic-5-Clause), which are used as
  dependencies;
- the stock-derived data (PixArt SROM, HyperX LED tables). It is not in the repository and is
  generated from your own dump.

HyperX and Pulsefire are trademarks of HP Inc. This project is not affiliated with HP,
HyperX, Kingston or PixArt.
