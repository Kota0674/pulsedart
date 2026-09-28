# What the firmware does

Source: `fw/mouse/src`. Built on Zephyr 4.4 / nRF Connect SDK v3.4.1, board `pulsedart`
(`fw/boards/hyperx/pulsedart`).

## Transports and modes

| When | Transport |
|---|---|
| USB cable plugged into a computer (host configured the device) | **USB** always wins |
| otherwise, wireless mode = ESB | **2.4 GHz** to the HyperX dongle |
| otherwise, wireless mode = BLE | **Bluetooth LE** |

The wireless mode is stored in flash (settings partition) and switched with a button combo.
The mouse blinks 3 times at full brightness on boot: **green** = dongle, **blue** = Bluetooth.

### USB
- Stock identity **0951:16E2** "Kingston / HyperX Pulsefire Dart", bcdDevice 0x1108, serial
  "000000000000", so NGENUITY recognises it (`CONFIG_PULSEDART_STOCK_USB_ID`).
- Four HID interfaces as stock: mouse, vendor (usage page 0xFF13, NGENUITY), keyboard,
  consumer.
- Polling 125/250/500/1000 Hz (stock setting).

### 2.4 GHz dongle (ESB)
- Nordic ESB, PTX, DPL, 1 Mbit/s, CRC16. Packet formats are compatible with the stock
  dongle: 0x60 mouse, 0x61 keyboard, 0x62 consumer, 0x78 keep-alive, 0xE0 tunnel reply
  (details: `re/radio.md`, `re/dongle_radio.md`).
- **Pairing record:**
  - By default the firmware uses the record the stock firmware left at flash 0xEF000, i.e. the
    dongle the mouse was paired with.
  - `CONFIG_PULSEDART_ESB_RECORD` in `fw/mouse/local.conf` (not in git) overrides it.
  - Pairing with L+R+DPI (the dongle in pairing mode) stores a new record in our settings.
- **Channel hopping:** the dongle moves the mouse to another channel after 8 ms of silence. The
  mouse follows this; after link loss it searches on channels 77 / 5, as stock.
- **NGENUITY over the dongle:** host commands arrive as ACK payloads (tunnel type 2), and replies
  go out as 0xE0 packets. Lost replies are resent up to 50 times.
- **Async status `FF 03`** (`re/status_ff03.md`): DPI stage, power state, "awake" and buttons.
  NGENUITY uses it to show the wireless mouse as online. The mouse sends it:
  - as stock: at boot, on wake, on every button edge, on power-state changes and after a
    factory reset;
  - additionally: after the link comes back, and then every 3 s until the host sends its first
    command. Without the extra sends, NGENUITY started while the mouse is quiet shows
    "Connection lost". After the first command the repeat stops, so host traffic does not keep
    the mouse awake.

### Bluetooth LE
- HID over GATT (mouse + keyboard + consumer reports) plus the battery service. Name
  "Pulsedart", up to 4 bonded hosts, Just Works pairing (encrypted, no MITM).
- Connection 7.5 ms interval, **peripheral latency 30**: no extra input lag. While nothing is sent
  the radio wakes only about every 0.23 s.
- **Advertising:**
  - fast (30–60 ms) for 30 s, then slow (1–1.2 s);
  - stops 3 min after the last disconnect and restarts on the next motion or button;
  - with a bonded host the reconnect after a wake takes a moment.
- **Link check:** a connection that is not encrypted or not subscribed to the mouse report within
  15 s is dropped, and advertising resumes (this protects against stale or stranger links).
- NGENUITY does not work over BLE (the stock mouse has no BLE, so NGENUITY has nothing for it).

## Controls

Hold combos for **5 s** (stock timing). For combos that include DPI, press DPI first.

| Combo | Action |
|---|---|
| DPI tap | next DPI stage (stock button map, default 800 → 1600 → 3200) |
| L + R + DPI | pair with a HyperX dongle (switches to ESB mode) - stock |
| M + DPI | factory reset of all settings (EEPROM defaults), white blink - stock |
| DPI + Forward | toggle wireless mode ESB ↔ BLE (reboots) - ours |
| DPI + Back | BLE only: forget all hosts and advertise - ours |

- Buttons pressed while DPI is held are combo keys and are not sent to the computer.
- A button already held before DPI (for example a drag) keeps working.
- A combo held across its own reboot does not fire again.

## Settings, lighting, buttons, macros (stock-compatible)

- **All user settings live in the external EEPROM in the stock layout** (`re/eeprom.md`). The
  stock firmware and ours read the same data, so switching firmware keeps the settings.
- **Lighting engine as stock** (`re/led_effects.md`):
  - per zone (logo, wheel): static, spectrum / colour cycle, breathing, reactive;
  - brightness and speed; custom LED arrays.
- **Indications:**
  - DPI change: the stage colour;
  - charging: white breathing;
  - low battery: red blinking;
  - pairing: green blinking;
  - mode switch / factory reset: 3 blinks.
- **Button map (6 buttons + wheel):**
  - mouse buttons, keyboard keys with modifiers, multimedia (consumer) keys;
  - DPI up/down/cycle and sniper;
  - macros (6 slots, played with the stock timing rules; stock macro bugs are fixed, see
    `re/actions_macros.md`).
- **NGENUITY protocol:**
  - all stock commands (`re/vendor_protocol.md`), byte-compatible replies;
  - verified live: detection, battery, lighting, polling rate, DPI, remapping, macros and
    saving (see [testing.md](testing.md)).

## Power

- **Active:** 1 ms main loop, sensor burst read, reports at the polling rate. On ESB a
  keep-alive goes out every 4 ms when nothing else is sent.
- **Idle / sleep** after about 10.5 s without motion, buttons or host activity, like stock:
  - the ESB radio is switched off, and NGENUITY shows the dongle mouse as offline until the next
    motion;
  - the LEDs are off;
  - nothing is polled. The main loop runs once a second (watchdog, battery check);
  - **level interrupts on MOTION, all 6 buttons and both wheel contacts wake it.** Measured wake
    to first packet at the dongle: 2–7 ms.
  - BLE keeps its connection with latency 30.
- **No System OFF.** It was tried and removed: waking from it meant a reboot and about 1 s
  before the first report.
- **External power** (USB cable or charger) keeps the mouse awake, as stock.
- **Battery:** read from the bq27421 (state of charge, voltage), reported over BLE (battery
  service) and to NGENUITY (`51 00`).
- **Fuel gauge setup as stock** (`re/power.md` §2.2). The gauge keeps its data memory in RAM,
  so it forgets it when the battery is drained or disconnected. About 1 s after boot, a
  background thread:
  - checks CHEM_ID 0x0128 and the "configured" marker (State block, offset 35 = 0xFF5D);
  - if the marker is missing, writes the stock values: 800 mAh, 3.0 V terminate voltage, Ra
    table, OpConfig 0x05F8. It uses the stock sequence (CFGUPDATE, block checksums, soft reset,
    seal).

  Start-up and wake are not delayed. The result goes to NGENUITY (`51 00` byte 9: 0 = OK,
  1 = no gauge, 2 = programmed on this boot). `51 01 1` (gauge Control) works as in stock.
- **Measured currents** (battery, fuel gauge, 1 mA resolution; see [testing.md](testing.md)):
  - moving, LEDs off: about 26 mA on BLE and 25 mA on ESB;
  - LEDs (static colour, full brightness): about +13 mA;
  - sleep: below the gauge's 5 mA floor.

## Memory map

The MCUboot variant (in use on the author's mouse) has this layout:
- MCUboot at 0x0–0xFFFF (with the USB recovery stack);
- slot 0 at 0x10000–0x7DFFF (our signed image);
- slot 1 at 0x7E000–0xEBFFF (update);
- 0xEE000 and 0xF0000 as in the table below.

See [flashing.md](flashing.md#mcuboot-variant-firmware-updates-over-usb-in-use-since-2026-09-28).

The normal build (stock boot stub kept):

| Range | Content | Written by |
|---|---|---|
| 0x00000–0x4FFFF | stock boot stub + HyperX USB bootloader | never (normal build) |
| 0x50000–0xECFFF | our application (slot `image-0`, about 260 KB used) | `flash_mouse.sh` |
| 0xEE000–0xEFFFF | stock data (0xEF000 = stock dongle pairing record) | never (read only) |
| 0xF0000–0xF7FFF | Zephyr settings (NVS): wireless mode, our pairing record, BLE bonds | firmware |
| external EEPROM | stock settings layout (lighting, DPI, buttons, macros, ...) | firmware, on save |

The boot stub starts 0x50000 if the header at 0x27000 says "jump". Our image is therefore
started by the untouched stock boot code.

## Source overview

| File | Role |
|---|---|
| `main.c` | main loop: inputs, combos, reports, status, sleep/wake, test hooks |
| `sensor.c` | PMW3389 init (stock sequence + SROM), burst motion read, CPI |
| `input.c` | buttons (debounce as stock) and wheel (quadrature, pull-ups only while sampling) |
| `actions.c/.h` | stock button map, macro player, DPI/sniper |
| `leds.c`, `leds_fx.c/.h` | PWM LEDs, stock lighting engine and indications |
| `stockcfg.c/.h` | stock EEPROM settings model (load, sanitize, save on demand) |
| `vendor.c/.h` | NGENUITY protocol (USB and dongle tunnel) |
| `usb.c` | USB device_next stack, 4 HID interfaces, vendor requests |
| `esb_link.c` | dongle link: ESB, channel handling, tunnel, status |
| `ble.c` | BLE HID, advertising budget, link check |
| `power.c` | charger pins, fuel gauge |
| `settings.c` | Zephyr settings: mode, pairing record |
| `hid_desc.h` | HID report descriptors (stock-identical mouse report) |

Debug hooks over SWD: [../re/fw_debug.md](../re/fw_debug.md).
