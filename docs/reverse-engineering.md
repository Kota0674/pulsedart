# Reverse-engineering notes (index)

The notes are in `re/`. They come from static analysis of the author's own flash dumps (stock
mouse firmware 1.1.0.8 and the dongle's nRF52810), checked on hardware where possible. Claims
are tagged **[C]** (confirmed in code or data) or **[H]** (hypothesis), with flash addresses.

## Mouse (stock firmware 1.1.0.8)

| File | Topic |
|---|---|
| [re/boot_layout.md](../re/boot_layout.md) | Boot stub, "jump" header at 0x27000, USB HID bootloader (0951:16F8), flash layout, update protocol |
| [re/sensor.md](../re/sensor.md) | PMW3389 driver: SPI, SROM upload, init sequence, burst format, DPI |
| [re/input_usb_led.md](../re/input_usb_led.md) | Buttons, wheel, USB descriptors (4 HID interfaces), LEDs, settings overview |
| [re/actions_macros.md](../re/actions_macros.md) | Button map types, keyboard/consumer reports, macro format and player (with the stock bugs) |
| [re/led_effects.md](../re/led_effects.md) | Lighting engine: effects, timing, tables (1:1 spec) |
| [re/power.md](../re/power.md) | Charger pins, fuel gauge, idle/sleep state machine, wake sources, WDT |
| [re/eeprom.md](../re/eeprom.md) | Settings EEPROM layout (I²C 0x50) |
| [re/vendor_protocol.md](../re/vendor_protocol.md) | NGENUITY protocol: every command, byte-exact replies |
| [re/status_ff03.md](../re/status_ff03.md) | Async "FF 03" status block (the wireless online flag for NGENUITY) |
| [re/radio.md](../re/radio.md) | 2.4 GHz link, mouse side: ESB config, packets, pairing, channel handling |

## Dongle (nRF52810 side) and the protocol

| File | Topic |
|---|---|
| [re/dongle_radio.md](../re/dongle_radio.md) | Dongle firmware: ESB PRX, pairing, channel search (8 ms silence), link-alive 1 s, ACK payloads, tunnel |
| [re/protocol.md](../re/protocol.md) | Consolidated mouse ↔ dongle protocol spec (enough to write an own dongle) |

The SONiX SN32F264 USB side of the dongle was not analysed.

## Our firmware: debug

| File | Topic |
|---|---|
| [re/fw_debug.md](../re/fw_debug.md) | SWD debug hooks |

## Tools

These scripts are in `re/tools/`. They run on the author's dump in `dump/`, which is not
published.
- `xref.py`, `callers.py`, `fn.sh`, `da.py`, `rdis.py`: disassembly and cross-references (capstone);
- `scatter.py`, `rwinit.py`: decompress the Keil RW data;
- `vendor_model.py`: a Python model of the NGENUITY handlers, used to compare our replies with
  stock;
- `gen_led_tables.py`: the LED tables (superseded by `tools/extract_blobs.py`).

Generated listings (`re/tools/out/`, `*.lst`, `dump/app.asm`) and binary extracts
(`re/*.bin`) are **not** published; see [publishing.md](publishing.md).
