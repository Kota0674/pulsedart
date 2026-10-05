# Dump, build, flash, restore

All commands run on the programmer (Raspberry Pi in `~/pulsedart`, a copy of `pi/`) unless
noted. The firmware is built on Windows (the author's setup); Linux works the same with
`west`.

## 0. Programmer setup (Pi)

```bash
sudo apt install openocd        # needs OpenOCD >= 0.12 with the linuxgpiod driver
scp -r pi/* <pi>:~/pulsedart/
cd ~/pulsedart && ./step1.sh    # read-only: IDCODE, FICR, APPROTECT state
```
`step1.sh` must report APPROTECT **open** (UICR 0x10001208 = 0xFFFFFFFF). If it is closed, stop:
unlocking would erase the chip and you would have no stock backup.

## 1. Dump the stock firmware

```bash
./dump_stock.sh
```
The script:
- reads flash (1 MB), UICR and FICR **twice** and compares the copies;
- reads the settings EEPROM (64 KB);
- writes `dump/flash.bin.sha256` and `dump/SHA256SUMS`.

Copy `dump/` to a safe place. It is your only way back to stock, and it must never be
published (see [publishing.md](publishing.md)).

## 2. Get the firmware files

The firmware needs two pieces of data from the stock firmware that cannot be published: the
PixArt sensor firmware (SROM) and the HyperX LED tables. Both come from your own dump. The
scripts accept only stock firmware 1.1.0.8 (they check a SHA-256 of the data).

### 2a. From a release, without a compiler

A release (`release/v<version>/`, built with `fw\build-release.ps1`) contains both firmware
variants with marker bytes where that data belongs:

| File | What |
|---|---|
| `pulsedart-stockboot.bin` | firmware for 0x50000, behind the stock boot code |
| `pulsedart-mcuboot.bin` | MCUboot for 0x0 |
| `pulsedart-app.signed.bin` | firmware image for MCUboot slot 0 (0x10000) |

```bash
pip install cryptography        # only needed for the MCUboot files
python tools/make_firmware.py --release release/v0.4.1 --dump dump/flash.bin --out out
```
The script:
- replaces the markers with the SROM and the LED tables from your dump;
- writes `out/fw.hex` for `pi/flash_mouse.sh`;
- for MCUboot, creates your signing key on the first run (`fw/keys/pulsedart-ec-p256.pem`, or
  `--key`), puts your public key into MCUboot and signs the image with your key. It writes
  `out/mcuboot.hex`, `out/app.signed.hex` (for `pi/flash_mcuboot.sh`) and `out/app.signed.bin`
  (for `pi/dfu_update.sh`).

Run it again on every new release with the same key. Lose the key and you need SWD again to
install a bootloader with a new one. With the files in `out/`, skip to step 4.

How this was checked: the output is byte-identical to a normal build of the same sources,
except for the build time in the version string, and MCUboot's own `imgtool verify` accepts the
signature (and rejects it for a different key).

### 2b. For your own builds

```bash
python tools/extract_blobs.py dump/flash.bin
```
This writes `fw/mouse/src/stock_blobs.c` (SROM and LED tables). The build stops with a hint if
the file is missing.

## 3. Build (Windows, nRF Connect SDK v3.4.1 in `C:\ncs`)

```powershell
fw\build.ps1 mouse                # -> fw\mouse\build\mouse\zephyr\zephyr.hex
```
`build.ps1` runs `west build -b pulsedart` inside the NCS toolchain with the board root
`fw/boards`. Output image: 0x50000 upwards, about 260 KB.

## 4. Flash

```bash
scp fw/mouse/build/mouse/zephyr/zephyr.hex <pi>:~/pulsedart/fw.hex
ssh <pi> 'cd ~/pulsedart && ./flash_mouse.sh fw.hex'
```
`flash_mouse.sh` refuses any image outside 0x50000–0xEE000 and checks that the chip on the
wires is the nRF52840. It erases only the sectors it writes and verifies them. It never touches:
- the stock boot stub,
- the HyperX USB bootloader,
- the stock pairing page 0xEF000,
- the UICR,
- the settings partition 0xF0000.

After flashing, run `openocd -f pulsedart_nolvl.cfg -f dbgoff.tcl`. This clears the SWD debug
power request; otherwise the chip stays in debug interface mode and draws extra current.

## 5. Logs

```bash
./rtt.sh                          # SEGGER RTT log over SWD (no CPU halt)
```
The RTT buffer is small. Messages logged while nobody reads are dropped, so start `rtt.sh`
before the event you want to see. Tools for live observation without halting the CPU:
- `linkwatch.tcl`, `wakewatch.tcl`: ESB link / sleep state;
- `readcfg.tcl`: stock settings in RAM;
- `curread.tcl`: battery current;
- `cmd.tcl`: debug commands.

These scripts contain RAM addresses of one particular build. Update them from
`arm-zephyr-eabi-nm zephyr.elf` after rebuilding (see [../re/fw_debug.md](../re/fw_debug.md)).

## 6. Restore the stock firmware

| Script | What it writes |
|---|---|
| `restore_stock.sh` | 0x50000–0xFFFFF from `dump/flash.bin` (stock app, data pages, settings area). Enough after the normal build. |
| `restore_full_stock.sh` | The whole 1 MB. Needed after the MCUboot variant (which replaces the stock boot stub at 0x0). |

Both verify `dump/flash.bin` against `dump/flash.bin.sha256` first. The EEPROM is not rewritten.
The stock firmware reads the same EEPROM layout that our firmware uses, so settings made with
our firmware carry over.

To put back a saved settings EEPROM (for example after a factory reset):
```bash
./eeprom_restore.sh dump/eeprom.bin
```
The script:
- reads the EEPROM and writes only the 128-byte pages that differ, through the chip's TWIM
  (CPU halted, peripheral reset first);
- resets the mouse, reads the EEPROM again and compares it with the image ("VERIFIED").

Tested on 2026-09-28: it restored the settings from before a factory reset.

## MCUboot variant: firmware updates over USB (in use since 2026-09-28)

Layout: MCUboot 0x0–0xFFFF, slot 0 0x10000–0x7DFFF (running image), slot 1 0x7E000–0xEBFFF
(update). Stock data 0xEE000 and settings 0xF0000 are unchanged.
- The mode is overwrite-only: MCUboot checks the ECDSA-P256 signature of slot 1 and copies it
  to slot 0.
- Images are signed with `fw/keys/pulsedart-ec-p256.pem`. **This key is private and must never
  be published.** Keep a backup: without it you cannot build updates for this bootloader.

Create your own signing key once (the key is not in the repository; `.gitignore` keeps
`fw/keys/` out):
```powershell
mkdir fw\keys
python C:\ncs\v3.4.1\bootloader\mcuboot\scripts\imgtool.py keygen -k fw\keys\pulsedart-ec-p256.pem -t ecdsa-p256
```

One-time switch over SWD:
```powershell
fw\build-mcuboot.ps1     # -> build-mcuboot\mcuboot\zephyr\zephyr.hex (bootloader)
                         #    build-mcuboot\mouse\zephyr\zephyr.signed.{hex,bin}
```
```bash
./flash_mcuboot.sh mcuboot.hex app.signed.hex   # erases 0x0-0xEC000, writes both, verifies
```

**Updates afterwards, over USB, no SWD** (from Linux or the Pi; the mouse is on a USB cable):
```bash
./dfu_update.sh zephyr.signed.bin
```
- Raise `CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION` in `fw/mouse/mcuboot/app.conf` for every release.
  The version is in the image header (slot 0 + 0x14).
- A corrupted or unsigned image is rejected by MCUboot, and the old firmware keeps running
  (tested).
- On Windows, dfu-util needs the WinUSB driver for the "Pulsedart DFU" device (2FE3:0101), for
  example installed with Zadig. It is not needed on Linux.
- NGENUITY's own "firmware update" is refused in this variant: there is no HyperX bootloader any
  more, and its header page 0x27000 now lies inside slot 0.
- `flash_mouse.sh` refuses to run while MCUboot is installed (it checks for the stock boot stub
  at 0x0). Back to the stock layout: `restore_full_stock.sh`.

**Recovery without SWD** (for a broken but signed image):
1. Connect the USB cable.
2. Switch the mouse off.
3. Hold **Left + Right + Back** (the rear side button, thumb) and switch it on. Keep holding for
   about 3 s, until the LEDs turn blue.

MCUboot then shows up as "Pulsedart recovery" (2FE3:0102, and 2FE3:FFFF in DFU mode), and
`./dfu_update.sh zephyr.signed.bin` writes slot 1. MCUboot checks and installs it and reboots
cleanly. MCUboot feeds the watchdog while it waits. The check costs nothing when the buttons are
not held, and waking from sleep never runs MCUboot. Code: `fw/mcuboot_hooks/`. Tested on
2026-09-28 (the power-on was done with an SWD reset while the buttons were held).

**Debug build with a USB log:**
```powershell
fw\build-mcuboot.ps1 -Debug     # -> build-mcuboot-debug\mouse\zephyr\zephyr.signed.bin
```
The build adds a CDC-ACM virtual COM port (no driver needed), and the log goes there as well as
to RTT. Flash it with `dfu_update.sh`, and open the port with any terminal at any baud rate
(Linux: `/dev/ttyACM0`). Go back to the normal build the same way.
