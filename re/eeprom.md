# Settings EEPROM (I2C 0x50) - live dump

`dump/eeprom.bin`, 64 KB, read twice over SWD-driven TWIM (identical),
sha256 `69897451391b35861414df8748f8bb766a0fc24d0043cd66d08eddece639be75`.
No address mirroring at 32/16/8/4 KB → a genuine 64 KB part (24x512 class).
Layout from `input_usb_led.md` §5 / `power.md`; values below are this unit's.

| Addr | Bytes | Meaning |
|---|---|---|
| 0x0001 | `08 00 01 01` | FW version 1.1.0.8 |
| 0x0010 | `07 00 07 04` | layout version 0x4707 |
| 0x0020 | `00` | polling-toggle state |
| 0x0060 | `00 00 0f ff ff ff 00 00 00` | misc/battery block, low-battery threshold 15 % |
| 0x0090 | 38 B | DPI block = factory defaults: step 2 (3200) active, steps 800/1600/3200/6400/16000, mask 0x07, sniper 800, colours blue/yellow/green/red/pink |
| 0x0110 | `01 02 20 20 00 00 32 ff ff ff 00 00 00` ×2 | LED zone 0 and zone 1: effect 1 (spectrum), params 02 20 20 00 00 32 |
| 0x0190 | 24 B | button map = factory defaults (L, R, M, back, fwd, DPI cycle, …) |
| 0x0210–0x026F | zeros | macro slots, empty |
| 0x0290 | 27 B | cfg: poll index 3 (1000 Hz), debounce bytes `03 12` (RAM uses 05/12 after boot), idle T1 = 10000 ms, idle T2 = 60000 ms |
| 0x0480–0x051F, 0x0680–0x071F | zeros | custom LED arrays zone 0/1, unused |
| 0x1488 | 3 × 8 B | pairing history: `02 02 6e 2f 15 41 01 ff` (×2), `02 02 6e 2f 90 c4 01 ff` (current) |
| 0x1690 | `02` | number of stored pairings |
| 0xFE00 | `00 40` | gauge re-program flag 0, INITCOMP wait counter 0x40 (skip) |

Notes
- The unit has been paired to two dongle addresses (`…15 41` and `…90 c4`); 0xEF000 in flash holds the current one.
- Our firmware does not write this EEPROM; this dump is the backup of the stock user settings.
