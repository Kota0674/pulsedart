# HyperX Pulsefire Dart (nRF52840 QIAAD0) - pin map

Consolidated from static analysis of the stock firmware 1.1.0.8 (`re/*.md`) and live SWD reads.
Details and addresses: `re/sensor.md`, `re/input_usb_led.md`, `re/power.md`, `re/radio.md`,
`re/boot_layout.md`.

## Sensor PMW3389 - SPI0, mode 3, 2 MHz
| Signal | Pin | Status |
|---|---|---|
| SCK | P0.22 | confirmed |
| MOSI | P0.17 | confirmed |
| MISO | P0.20 | confirmed |
| NCS | P0.13 | confirmed (GPIO-driven) |
| MOTION | P0.15 | confirmed (polled, active low, wake source) |
| P1.07 | output, driven high once at init, never low | role unknown (sensor power/reset?) |
SROM: `re/srom_pmw3389.bin` (4094 B, SROM_ID 0x05). Init sequence: `re/sensor.md`.

## Buttons - pull-up, active low, polled every ~1 ms, debounce 5 ms press / 18 ms release
| Pin | Function (stock default mapping) |
|---|---|
| P1.15 | Left |
| P1.13 | Right |
| P1.10 | Middle |
| P1.00 | DPI |
| P1.04 | Button 5 (forward), front side button (confirmed 2026-09-27) |
| P1.02 | Button 4 (back), rear side button (confirmed 2026-09-27) |
Combos (~5 s): L+R+DPI pairing, M+DPI factory reset, L+R+M (USB, after unlock) bootloader.

## Scroll wheel
P0.02 / P0.29 quadrature, polled every 1 ms, pull-ups enabled only while sampling, 1 count/detent.
Direction confirmed on hardware (no inversion needed).

## RGB LEDs - active high, PWM 1 MHz / top 320 (3.125 kHz)
| Zone | R | G | B |
|---|---|---|---|
| 0 | P0.08 | P0.04 | P0.06 |
| 1 | P0.12 | P0.11 | P1.09 |
P0.24 - LED supply enable (high whenever the LEDs are driven; confirmed by SWD readout).

## I2C - TWI1, SCL P0.00, SDA P0.01, 400 kHz (no 32 kHz crystal; LFCLK = RC)
| Addr | Device |
|---|---|
| 0x50 | settings EEPROM (~64 KB, 16-bit addr, 128 B pages) |
| 0x55 | TI bq27421 fuel gauge (SOC reg 0x1C, voltage reg 0x04, 800 mAh design) |

## Power / charging
| Pin | Role (hypothesis unless noted) |
|---|---|
| P1.11 | charger power-good, active low (0 = external power) - confirmed |
| P1.06 | charger CHG status, active low (0 = charging) - confirmed; stock wake source |
| P0.14 | Qi / wireless power present |
| P0.10 | Qi receiver enable (off when full) |
| P0.31 | fuel gauge GPOUT / wake |
| P0.09 | USB cable detect? (reads 1 on battery) |
| P0.25 | factory RF-test strap, high at boot = test mode (confirmed) |
No power-latch pin: MCU is powered from the battery via VDDH (REG0 = 2.1 V), DCDC on REG1.

## Flash layout (stock)
| Range | Content |
|---|---|
| 0x00000–0x004F0 | boot stub: boots 0x50000 if word @0x27010 = "jump" and 0x50000 not erased, else bootloader |
| 0x27000 | boot header (range, status, CRC-16/CCITT-FALSE) |
| 0x28000–0x327AF | USB HID bootloader (VID 0951 PID 16F8) |
| 0x50000–0x6719C | application (max 0x9D000) |
| 0xEE000 | 2 bytes written, never read |
| 0xEF000 | radio pairing record: ch 2, dongle addr 6e 2f 90 c4, paired |
Interrupts are forwarded by the stub (0x220) unless the app sets VTOR itself.
