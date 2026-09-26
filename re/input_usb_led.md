# Stock firmware: input, USB, vendor protocol, LEDs, settings

Source: static analysis of `dump/flash.bin` (app 0x50000–0x68000).
Tags: **[C]** = CONFIRMED (seen in code or data at the address given). **[H]** = HYPOTHESIS (inferred, not proven).

Helper files written for this analysis (all under `re/tools/`):
- `iul_dis.py START END` disassembles a range with literal annotation. `iul_app.lst` is the full listing of 0x50000–0x64700. `iul_cmd.lst` covers the vendor-command handler 0x51c44–0x53554.
- `iul_rwdata.py` decompresses the ARMCC scatter-load RW image into `rw_20002210.bin`. The table at 0x66f24 gives src 0x66fbc, dst 0x20002210, len 0x624, and handler 0x50520 (LZ-style `__decompress`). A zero-init region follows at 0x20002834 (len 0x7cdc). **[C]** All RAM defaults quoted below come from this decompressed image.

Common RAM variables:

| RAM | Meaning |
|---|---|
| 0x200076c4 | "cfg" block, 0x1b bytes |
| 0x200076df | misc block, 9 bytes |
| 0x200076e8 | LED zones, 2×13 bytes |
| 0x20007702 | DPI block, 0x26 bytes |
| 0x20007728 | button map, 8×3 bytes |
| 0x20007740 | macros, 6×0x209 bytes |
| 0x20008385 / 0x20008415 | custom LED arrays, 0x90 bytes each |

---

## 1. Buttons

### 1.1 Pins and scan  [C]
- Pin table @0x66930: `2f 2d 2a 24 22 20 | 09 2b 26 1f 0f 0e`. The first 6 entries are the buttons. The rest are other wake/sense pins: P0.09, P1.11, P1.06, P0.31, P0.15 (sensor MOTION), P0.14.
- Pull table @0x6693c: `03 03 03 03 03 03 ...`. All buttons use pull-up (PULL=3), pressed = 0.
- Scan function `0x61ab0`. It runs from app_timer #1 (instance 0x648b8, handler 0x618e9 → app_sched → 0x61ab0).
  - The timer is started with 0x21 = 33 ticks (0x5a3dc, 0x5fadc). RTC1 prescaler is 0 (0x54f60 → 0x607c0 writes 0x40011508), so the tick is 32768 Hz and the **scan period is 1.007 ms**.
  - Each scan reads the 6 pins with `0x5cd64` (reads `IN`, offset 0x510). Button i sets bit i of the raw mask when the pin is low.
- P1.06 is **not** a button. The scan loop stops at 6 entries. P1.06 only appears in the sleep/wake config (0x56d90: input, pull-up, SENSE=low). It is a wake source.

| idx | Pin | Raw bit | Default function (from default map, §1.3) |
|---|---|---|---|
| 0 | P1.15 | 0x01 | Left (HID bit0) |
| 1 | P1.13 | 0x02 | Right (HID bit1) |
| 2 | P1.10 | 0x04 | Middle / wheel click (HID bit2) |
| 3 | P1.04 | 0x08 | mouse code 4 → **HID bit4 (0x10) = "Button 5", usually Forward** |
| 4 | P1.02 | 0x10 | mouse code 5 → **HID bit3 (0x08) = "Button 4", usually Back** |
| 5 | P1.00 | 0x20 | **DPI button** (special type 7, code 8 = DPI cycle up) |
| 6 | (wheel −) | – | code 0xE8 (wheel +1) |
| 7 | (wheel +) | – | code 0xE9 (wheel −1) |

- The code→HID-bit table @0x66953 is `01 02 04 10 08`, so codes 4 and 5 are swapped relative to the bit order. [C]
- Which physical side button is P1.04 versus P1.02 can only be settled by pressing them. By HID convention P1.04 is Forward and P1.02 is Back. [H]
- The code contradicts the PINMAP.md guess: P1.00 is the DPI button, not a side button. [C]

### 1.2 Debounce  [C]
- Per-button counters are at 0x200024b8[i]. The stable mask is at 0x200024d8.
- While raw ≠ stable, the counter increments once per 1 ms scan. When raw == stable, the counter resets to 0 (0x61ba8). The state flips when counter ≥ threshold.
- Press threshold = cfg[0x0f]. Release threshold = cfg[0x10]. At boot these are forced to **5 ms (press) and 18 ms (release)** (0x5a276..0x5a27e).
- Both thresholds get +5 while `0x200024b0 < 10`. 0x200024b0 is byte 6 of the sensor burst buffer 0x200024a4, i.e. SQUAL. [C]
  - Likely meaning: longer debounce when the mouse is lifted or on a poor surface. [H]
- The host can change the thresholds with vendor command `D0 54 78 xx press rel`. The value is RAM only, and it is reset at the next boot.
- On a stable-state change the firmware builds an event from `0x58988` (map lookup) and sends it via `0x59b60` (0x61b84–0x61b9e).

### 1.3 Button map (RAM 0x20007728, 8 entries × 3 bytes: type, code, aux)  [C]
Defaults (RW 0x200027f1): `01 01 01 | 01 02 02 | 01 03 04 | 01 04 08 | 01 05 10 | 07 08 00 | 01 e8 00 | 01 e9 00`.

Event dispatcher is `0x58370`:

| type | meaning |
|---|---|
| 1 | Mouse button. Code 1..5 → HID bit via table 0x66953 (1→0x01, 2→0x02, 3→0x04, 4→0x10, 5→0x08). Code 0xE8 = wheel +1 and 0xE9 = wheel −1 (accumulator 0x200024c8). |
| 2 | Keyboard key. The HID usage is the aux byte; up to 6 keys are tracked in 0x200024c9. |
| 3 | Consumer key. `code & 0xf` indexes table @0x6694c: 0 Play/Pause 0xCD, 1 Stop 0xB7, 2 Prev 0xB6, 3 Next 0xB5, 4 Mute 0xE2, 5 Vol− 0xEA, 6 Vol+ 0xE9. |
| 4 | Macro. Code = macro index 0..5, stored at 0x20007740 + i×0x209. |
| 7 | Special. 6 = DPI up (no wrap), 7 = DPI down (no wrap), 8 = DPI cycle up (wrap), 9 = DPI cycle down (wrap) (0x62e34 arg 0..3); 0x0b = DPI-shift/sniper while held (DPI = 0x20007702+8, 0x58816). |
| other | No action. [H] Type 0 = disabled. |

- The aux byte is computed by `0x50c04(type, code)`: type 1 gives 1<<(code−1), type 3 gives 1<<code, otherwise the code itself.

### 1.4 Button combos (all held ≥ 5000 scans ≈ 5 s, 0x61c48..0x61cba)  [C]
- The stable mask must equal the combo exactly (`0x200024c4 == mask`). The hold counter is 0x200024e4 with limit 0x1388.
- While a combo is running, 0x200024c3 = 1 suppresses normal button reports, and 3 empty reports are queued (0x200024c5 = 3).

| mask | Buttons | Result |
|---|---|---|
| 0x23 | L + R + DPI | Event 7, then event **6** about 49 ms later. Event 6 = **enter pairing mode** (0x51b84: `0x2000223d=1`, LED pairing blink, pairing timeout 0x20002254 = 500 or 0xffff). Only when `0x59aa4()==0` (0x2000246a clear). |
| 0x18 | both side buttons (P1.04 + P1.02) | Event 9, then event 8 after about 49 ms. **No consumer of event 8 was found.** [open] |
| 0x24 | Middle + DPI | Event **10** = **factory reset**. `0x53ad4` copies RW defaults into RAM, then 0x5a8ee writes every block to the EEPROM. One shot, latched by flag 0x200024e9. |
| 0x07 | L + R + M | Event **4** = **jump to DFU bootloader** (0x63efc → 0x56cd8). Requires USB cable present (0x20002469), 0x2000246a set, **and** DFU unlocked by vendor cmd `A0 EA 5A A5` (0x20002811). |

- Separately: when P0.09 reads 0 and flag 0x2000229c is set, the scan triggers `NVIC_SystemReset` (0x61ce6). This belongs to the power/charger logic. [C]

### 1.5 Idle / sleep timers (in the scan)  [C]
- Counter 0x200024e0 counts idle scans. Activity resets it via `0x5661c`.
- After cfg[0x11] = 0x2904 (10 500 ms) of idle: stage 1 (`0x2000224c=1`).
- After a further cfg[0x15] = 60 000 ms: stage 2 (`0x2000224c=2`, `0x200024d6=1` = sleep request).
- Stage 2 only happens if 0x2000280a is set (`D0 54 87 01`), no USB power is present, and 0x200024fb ≠ 2.
- Meaning [H]: stage 1 = LED off/dim, stage 2 = deep sleep (System OFF, woken by the sense pins).

---

## 2. Scroll wheel (P0.02 = A, P0.29 = B)  [C]
- Method: **GPIO polling** from the 1 ms button scan (`0x589d8 → 0x641ac → 0x64104`). No QDEC, no GPIOTE and no PPI are used while awake.
- Each poll:
  1. Configure both pins as input with pull-up (`0x5c97c(pin,3)`).
  2. Busy-wait about 10 µs (`0x66991` delay loop, r0 = 640).
  3. Read A into bit0 and B into bit1.
  4. Disconnect both pins again (`0x5c930`). This saves pull-up current.
- The state byte 0x200024f6 holds `(prev<<2 | cur)`. Transition table @0x66980: `00 01 02 03 02 00 03 01 01 03 00 02 03 02 01 00` (1 = step dir A, 2 = step dir B, 3 = invalid/reset).
- **Two valid consecutive steps in the same direction give 1 count**, so each detent is half a quadrature cycle (00↔11).
  - Direction 1 is the sequence 00→01→11→10→00 (A leads B). It gives count +1 in 0x200024f5.
- Count > 0 → button-index 7 → code 0xE9 → wheel −1. Count < 0 → index 6 → 0xE8 → wheel +1. The wheel byte is 0x200024c8, sent in mouse report byte 5.
  - So "A leads B" = scroll down (negative HID wheel). [C] for the logic, [H] for the physical direction.
- In sleep, P0.02 and P0.29 are given a SENSE level opposite to their current level, so the wheel wakes the MCU (0x56d96..0x56dd0).

---

## 3. USB (USBD, nRF5 SDK `app_usbd` + `app_usbd_hid_generic`)

### 3.1 Device descriptor @0x66d90  [C]
`12 01 00 02 00 00 00 40 51 09 e2 16 08 11 01 02 03 01`

| Field | Value |
|---|---|
| bcdUSB | 2.00 |
| EP0 max packet | 64 |
| **VID** | **0x0951** (Kingston) |
| **PID** | **0x16E2** |
| **bcdDevice** | **0x1108** |
| iManufacturer / iProduct / iSerial | 1 / 2 / 3 |
| Configurations | 1 |

### 3.2 Configuration and strings  [C]
- Config header @0x66da2: `09 02 .. .. .. 01 00 C0 FA`. wTotalLength and bNumInterfaces are filled at run time. bConfigurationValue = 1, attributes 0xC0 (self-powered, no remote wakeup), MaxPower 0xFA = 500 mA.
- String descriptors @0x66de0:
  - 0 = LANGID 0x0409
  - 1 = "Kingston"
  - 2 = "HyperX Pulsefire Dart"
  - 3 = "000000000000" (serial)
  - 4 = "User 1"
  - A RAM copy is at 0x200026e4. [H] The serial is constant; no FICR-based serial code was found.
- Endpoint descriptors (hid_feed_descriptors 0x5923c..0x593a0): `07 05 EP 03 40 00 01` = interrupt, 64 bytes, **bInterval = 1**. [C]

| IF | Instance | EPs | Subclass / protocol | Report descriptor |
|---|---|---|---|---|
| 0 | 0x647d4 | 0x81 IN | 1 (boot) / 2 (mouse) | Mouse @0x646f8, 64 B |
| 1 | 0x64804 | 0x82 IN, 0x03 OUT | 0 / 0 | Vendor @0x64744, 25 B (event handler 0x58afd) |
| 2 | 0x6483c | 0x84 IN | 0 / 1 | Keyboard @0x6476c, 47 B |
| 3 | 0x64874 | 0x85 IN | 0 / 0 | Consumer @0x647a8, 23 B |

### 3.3 Report descriptors and report formats  [C]
- **Mouse**, no report ID, 6-byte report. The layout is filled by `0x60248`:
  - byte0 = buttons (5 used)
  - byte1–2 = X (int16)
  - byte3–4 = Y (int16, logical range −32767..32767, relative)
  - byte5 = wheel (int8, −127..127)
  - Descriptor: `05 01 09 02 a1 01 09 01 a1 00 05 09 19 01 29 05 15 00 25 01 75 01 95 05 81 02 75 01 95 03 81 01 75 10 95 02 05 01 09 30 09 31 16 01 80 26 ff 7f 81 06 09 38 15 81 25 7f 95 01 75 08 81 06 c0 c0`
- **Vendor**: `06 13 ff 09 01 a1 01 15 00 26 ff 00 75 08 95 40 09 02 81 02 09 03 91 02 c0`. Usage page 0xFF13, 64-byte IN (usage 2) and 64-byte OUT (usage 3), no report ID. NGENUITY uses this interface.
- **Keyboard**: standard 8-byte boot-style report (modifier, reserved, 6 keys, keys 0..0xFB, array). No LED output report.
- **Consumer**: one 16-bit usage 1..0x2FF, array.

### 3.4 Report timing / polling rate  [C]
- Reports are sent from the **SOF event** of the app_usbd user handler `0x63d50` (event 0, 0x63d70). SOF count 0x20002476 is compared with `interval = table@0x66f20[cfg[0x0e]]`.
- Table values `08 04 02 01` → 125 / 250 / 500 / 1000 Hz. cfg[0x0e] default = 3 (1000 Hz).
- Boot protocol (instance byte 0x11 == 0) forces interval 8.
- `0x60248` returns 1 = mouse, 2 = keyboard, 3 = consumer. The matching senders are 0x5ac48, 0x59bbc and 0x56b7c.

### 3.5 Wired vs wireless  [C]
- app_usbd events handled in `0x63d50` (tbb at 0x63d5c):

| Event | Action |
|---|---|
| POWER_DETECTED (8) → 0x63f1e | `0x20002469=1` (VBUS present), restart app_timer 2 (0x4000 ticks = 0.5 s) |
| POWER_REMOVED (9) → 0x63f3e | `0x20002469=0`, restart timer 2 |
| SUSPEND / RESUME (2 / 3) | Toggle 0x20002475 |

- Timer-2 handler `0x61f64` runs after the 0.5 s debounce:
  - **VBUS gone → `NVIC_SystemReset`** (0x61fbc..0x61ff6). The mouse always reboots into wireless mode when the cable is removed.
  - VBUS present and USB suspended → write 1 to 0x40000638, run the sleep routine `0x56e2c` (button/wheel sense wake), then write 0 to 0x40000638.
  - VBUS present, not suspended → start timer 6 (0x20000 = 4 s), set 0x200024e7 = 1, restart scan timers (0x5fad4).
- While 0x20002469 is set, reports go out over USB (SOF path).
- The radio report builder is `0x5ea44`. It builds packet types 0x60 mouse / 0x61 keyboard / 0x62 consumer into buffer 0x20002834. [H] It is used when USB is absent; details belong to the radio report.
- `0x589c0` returns 0 (no sleep) when USB power is present.

---

## 4. Vendor (NGENUITY) protocol  [C]
- **Transport**: 64-byte OUT report on IF1 / EP3. `0x58b00` (event 2) reads it into 0x20005e68 and calls `cmd_handler(buf, out=0x20005e28, len, 0, source=1)` at 0x51c44.
  - If the handler returns ≠ 0, the reply is sent as a 64-byte IN report on EP 0x82 (`0x55cee`).
  - The same handler is called from the radio path at 0x50682 (source=0, reply length byte → 0x20002327). So the dongle can tunnel these commands.
- **Reply** = the request buffer modified in place: `[0]=cmd [1]=sub [2]=idx [3]=len [4..]=data`.
  - On error: `[3]=0x02 [4]=0xEC [5]=err`. err 1 = bad sub, 2 = bad index, 3 = bad byte3, 4 = value out of range, 0 = unsupported command (0x53304..0x53516).
  - Write commands echo the request.
  - Some commands set a deferred action (0x20002818=1). They then return 0, so no immediate IN report is sent.
- Commands with `(cmd & 0xD0) == 0xD0` are writes, dispatched by the tbh at 0x51c90. All other commands are handled at 0x527a4 and 0x5335a.

### 4.1 Write commands (RAM only; persist with `DE`)

| Cmd | Payload | Effect |
|---|---|---|
| `D0 00 .. .. p` | p < 4 | cfg[0x0e] = polling index (0 = 125 Hz … 3 = 1000 Hz) |
| `D0 01 z 0/1 n` | z ∈ {0,1}, n ≤ 48 | cfg[0x19+z] = n×3: number of custom-LED RGB entries for zone z |
| `D0 02 z cnt start rgb…` | cnt < 20, start < 48 | Copy cnt RGB triplets into custom array z (0x20008385 / 0x20008415) at start×3 |
| `D0 54 78 .. p r` | – | Debounce press / release (cfg[0x0f] / cfg[0x10]) |
| `D0 54 87 .. e` | – | 0x2000280a = (e ≠ 0): allow deep sleep (§1.5) |
| `D0 AA 55 D0 AA 55` | – | Reset both LED zones: brightness 100, effect 0, colours ff; re-apply; also writes sensor reg 0x10 = 0 (0x5fe0e) |
| `D1 00 .. .. v` | 5 ≤ v ≤ 25 | 0x200076df[2] = v = **low-battery threshold %** (default 15). [H] meaning, from its use at 0x5a894 |
| `D1 94 78 .. v` | – | Same, without range check |
| `D1 01` / `D1 02` | – | EEPROM state machine 0x20002810 = 5 / 6: EEPROM 0xFE00 := 1, call 0x516d8, clear counter 0xFE01. [H] Debug |
| `D2 zz eff_mode c1R c1G c1B c2R c2G c2B brightness speed` | zone = zz>>4 (0, 1, or 2 = both); eff = mode>>4 (<5); sub = mode&0xf (<4); brightness ≤ 100 | LED zone set: zone[0]=eff, zone[1]=sub, zone[2]=speed ([0xb]), zone[6]=brightness ([0xa]), zone[7..12]=two colours ([4..9]). Applied via 0x5eee0 |
| `D3 00 .. .. s` | – | Select DPI stage s (<5, must be enabled). Writes the sensor via 0x6097c |
| `D3 01 00 .. mask` | mask ≤ 0x1F | Enabled-stage bitmask ([7]); rebuild the stage list 0x2000250e |
| `D3 02 s .. lo hi` | – | DPI[s] = u16, range 2..320. **Unit = 50 CPI** (100..16000) |
| `D3 03 s .. R G B` | – | Colour of DPI stage s |
| `D3 04 00 .. lo hi` | 2..320 | DPI-shift (sniper) value |
| `D4 b t .. c` | b < 6, t < 8 | Button b map = (type t, code c, aux = f(t,c)) |
| `D5 m 00 .. lenlo lenhi mode rep` | m = macro 0..5; len ≤ 0x200; mode ≤ 3 | Macro header ([0..1]=len, [6]=mode, [7..8]=repeat if mode==1) |
| `D6 m o_hi o_lo|n …events` | – | Macro data. Skip `o = (b2<<2)|(b3>>6)` existing events, then write `n = b3&0x3f` (≤ 6) events. Each event is 5 bytes, or 10 bytes if its first byte is 0x1A. Counters: macro[2] = #5-byte events, macro[4] = #10-byte events |
| `DE k` | k ∈ 0..5 or 0xFF | **Save to EEPROM** when idle (0x53b64): 0 = cfg + custom LED, 1 = misc, 2 = LED zones, 3 = DPI, 4 = buttons, 5 = macros, FF = all |
| `DF AA k` | k ∈ 0..5 or 0xFF | **Load factory defaults** for that block (same numbering; 0 also clears the custom LED arrays) |

- `DF` sub ≠ 0xAA → error 1.
- `D7..DD` → error.

### 4.2 Read / info commands

| Cmd | Reply |
|---|---|
| `50 00 00` | [3]=0x1d, [4..5]=`E2 16` (PID), [6..7]=`51 09` (VID), [8..11]=`08 00 01 01` (FW 0x01010008), [12..]="HyperX Pulsefire Dart\0" (ASCII @0x529e4) |
| `50 00 01` | [4..11] = `02 0e 01 05 08 00 01 01` ([4..7] meaning unknown) |
| `50 00 D0` | [3]=1, [4] = EEPROM 0x1690 (number of stored pairings) |
| `50 00 D1 n` | [3]=8, [4..11] = pairing record n from EEPROM 0x1490+8n |
| `50 00 D2` | Deferred: save pairing record |
| `50 01` / `50 02` | Stream custom LED array of zone 0 / 1 in 20-entry chunks ([3] = entries). Continuation is sent from the EP-IN-done event 0x58b58. `AC EC 01` in [4..6] continues a pending read |
| `50 03` | [3]=0x31. [4]=zone0 effect, [5]=zone1 effect, [6]=#enabled DPI stages, [7..8]=sniper DPI, [9..18]=5×DPI u16, [19..33]=5×RGB, [34..51]=6×button map, [52]=polling index |
| `51 00 00` | [3]=3. [4..6]=0x200076df[0..2], [7..8]=u16 [6], [9]=[8], [10..13]=u32 0x20002288 (EEPROM 0xFE01) |
| `51 01 0/1 .. r` | u32 at [5..8] from TWI device 0x20006614 register r (0x57824 / 0x57878). [H] Debug access to the I²C part |
| `52 00 00` | [3]=0x11. Zone0: [4]=eff, [5]=sub, [6]=speed, [7]=brightness, [8..13]=2×RGB. Zone1: [14..17], [18..23] |
| `53 00 00` | [3]=0x21. [4]=current stage, [5]=enable mask, [6..7]=min (2), [8..9]=max (320), [10..11]=sniper, [12..21]=5×DPI, [22..36]=5×RGB |
| `53 94 87 .. r` | [5] = sensor register r (0x539ac → 0x5fd52), then writes reg 0x50 = 1 |
| `54 00 00` | [3]=0x0c, [4..21] = 6×3 button map |
| `55 m` | Macro header ([4..5] len, [6] mode, [7..8] repeat). **Bug: index = m & 0xF0**, so only macro 0 is readable |
| `56 m o …` | Macro data read (same index bug) |
| `57 13 01 00` | [3]=8, [4..10] = 7-byte pairing/dongle ID at 0x20002232 |
| `07` | Error reply with err 0 |

### 4.3 Bootloader / DFU commands  [C]
- `A0 EA 5A A5` unlocks (0x20002811=1). `A0 DA 5A A5` locks. Reply `[2..3]=0002 [4]=EC [5]=AC` (ok) or `FE` (fail).
- `A1 00 4B B4 94 10 98 27 24 10 00 01` (only when unlocked) → `0x56cd8`: **enter DFU**.
  - The routine copies the 0x1c-byte header at flash 0x27000 (`00 00 28 00 | 00 00 05 00 | 00 00 05 00 | 9c 71 06 00 | "jump" | ...`).
  - It sets word +0x10 ("jump" magic) to 0xFFFFFFFF, rewrites page 0x27000, waits 300 ms and resets.
  - So 0x27000 is the bootloader settings page. "jump" = boot the app.
- `A2 10` (unlocked) → [2..3]=4, [4..7] = word at flash 0x50200 (0x02000965). [H] Build/version tag.
- `A2 13 01 00` → 7-byte pairing ID.
- `A4 B0 C1 EA id0..id6` (unlocked, USB source only) → sets the pairing ID 0x20002232 and starts EEPROM state 1 (apply to radio).

---

## 5. Settings storage: external I²C EEPROM on TWI1 (not internal flash)  [C]
- `0x50c40(addr16, buf, len, write)` sends a 16-bit big-endian address, then data. Writes wait 30 ms.
- Flash page 0xEF000 only holds 7 bytes (`02 02 6e 2f 90 c4 01`), read at 0x5a238 into 0x20002232.
  - This means the TWI1 device at P0.00/P0.01 is (at least) an EEPROM. [C] PINMAP calls it the "Qi receiver U10", so it may be a combined part or a separate chip. [open]

| EEPROM addr | Len | RAM | Content |
|---|---|---|---|
| 0x0001 | 4 | – | FW version nibbles (`08 00 01 01`) |
| 0x0010 | 4 | – | Layout version (0x4707 as nibbles `07 00 07 04`). On mismatch, defaults are rewritten (0x59fa6) |
| 0x0020 | 1 | 0x20002809 | Polling-toggle state |
| 0x0060 | 9 | 0x200076df | Misc: [2] = low-battery %, [0] / [6] = battery info. [H] |
| 0x0090 | 0x26 | 0x20007702 | DPI block |
| 0x0110 | 0x1a | 0x200076e8 | LED zones |
| 0x0190 | 0x18 | 0x20007728 | Button map |
| 0x0210+16i | 9 | macro i header | – |
| 0x0290 | 0x1b | 0x200076c4 | cfg |
| 0x0490 / 0x0500 | 0x70 / 0x20 | 0x20008385 | Custom LED zone 0 |
| 0x0690 / 0x0700 | 0x70 / 0x20 | 0x20008415 | Custom LED zone 1 |
| 0x0890 + 0x200·i | 0x200 | macro i data | – |
| 0x1490 + 8n | 8 | – | Pairing records |
| 0x1690 | 4 | – | Pairing count |
| 0x16A0 | 1 | – | Pairing flag |
| 0xFE00 | 1 | – | [H] Debug flag |
| 0xFE01 | 1 | 0x20002288 | [H] Debug counter |

**Default blocks** (RW image):
- cfg @0x2000278d: `01 01 00 08 00 01 00 00 00 01 00 00 00 03 03 03 12 10 27 00 00 60 ea 00 00 00 00`
  - [0x0e] = 3 (1000 Hz), [0x0f] / [0x10] debounce (overridden to 5 / 18 at boot).
  - [0x11..0x14] = 10000, overridden to 10500. [0x15..0x18] = 60000.
  - [0x00..0x0d] unknown.
- DPI @0x200027cb:
  - [0] = stage 0, [2] = 3 enabled stages, [3..6] = min 2 / max 320, [7] = mask 0x07, [8..9] = sniper 0x10 (800).
  - DPI values = 800, 1600, 3200, 6400, 16000.
  - Colours: **blue 0000FF, yellow FFFF00, green 00FF00, red FF0000, pink FFC0CB**.
- LED zones @0x200027b1 (both zones): `01 02 20 20 00 00 32 ff ff ff 00 00 00` = spectrum cycle, speed 0x20, brightness 50 %, colours white / black.
- Misc @0x200027a8: `00 00 0f ff ff ff 00 00 00` (low battery = 15 %).

---

## 6. RGB LEDs  [C unless marked]

### 6.1 PWM setup
- Init is `0x5f220`, with two nrfx_pwm instances: 0x20002418 = PWM0 and +8 = PWM1.
- Config struct @0x646cc: irq_prio 7, **base clock 4 = 1 MHz**, count mode Up, **COUNTERTOP overridden to 0x140 = 320**, so the **PWM frequency is 3.125 kHz**.
- Load mode = Individual (2), step = Auto.
- Sequence: 4 × u16 values at 0x20005d18 (PWM0) and 0x20005d20 (PWM1), length 4, played with `NRFX_PWM_FLAG_LOOP`.
- Pin table @0x646c4: PWM0 OUT0..3 = P0.12, P0.11, P1.09, P0.08; PWM1 OUT0..1 = P0.04, P0.06. There is **no 0x80 inversion flag on any pin**.

### 6.2 Setting a channel and polarity
- `0x5f1a0(pin, v)` finds the channel from the pin and stores `compare = 320 − v` (polarity bit 15 = 0).
  - With polarity 0 the output is low until COMPARE, then high. The high-time fraction is therefore v/320.
- The LED-off routine `0x5f2f4` stops the PWM and drives every LED pin **low** (write to OUTCLR +0x50C, pin = output).
- Init values are 320, which also means off.
- Conclusion: **LEDs are active-high**, brightness ∝ v, and the maximum duty is 255/320 ≈ 80 %.

### 6.3 Colour mapping (`0x64428(zone, brightness 0..100, rgb[3])`)
- Scale = brightness×256/101, and each channel = rgb×scale>>8. Channel→pin table @0x66c14:
  - **Zone 0: R = P0.08 (PWM0.3), G = P0.04 (PWM1.0), B = P0.06 (PWM1.1)**
  - **Zone 1: R = P0.12 (PWM0.0), G = P0.11 (PWM0.1), B = P1.09 (PWM0.2)**
- The RGB byte order R, G, B is confirmed by the DPI default colours (the known HyperX 800 = blue, 1600 = yellow, 3200 = green scheme).
- Which zone is the logo and which is the wheel is unknown. [open]
- P0.24 is configured as output and set to 1 at the end of every LED-indication routine (0x51124, 0x50b10, 0x51168). [H] It is the LED power / boost enable.

### 6.4 Effects
- Per-zone state is at 0x200060f4 + zone×0x10. Zone 0 uses app_timer 4 and zone 1 uses app_timer 5.
- The dispatcher is 0x5eee0.
- The HSV→RGB routine 0x59d9c uses the 256-byte gamma table @0x66996. Triangle/breath tables are at @0x66a93..0x66bf0; `0x58348` scales RGB by table 0x66b52.

| zone[0] | Handler | Behaviour |
|---|---|---|
| 0 | 0x60a3c | Static colour1 at the zone brightness [C] |
| 1 | 0x56720 | **Spectrum cycle**: HSV with S = V = 255, hue stepping; timer 0x400 ticks = 31 ms [C] |
| 2 | 0x5636c | Uses colour1 and speed. Step period = (speed+16) ms [C]. [H] Breathing |
| 3 | 0x5ff88 | Uses colour1 and speed. [H] Trigger-fade (reacts to clicks) |
| 4 | – | Allowed by D2; not in 0x5eee0. [H] Custom array (D0 02) |

### 6.5 System indications
- **DPI change**: `0x50b10(stage)` shows the stage colour (0x20007719 + 3·stage) on both zones in mode 7 at brightness 0x32, then the effect resumes. The timers use 0x3d7 ticks ≈ 30 ms steps.
- **Low battery**: `0x51168(1|2)`. Mode 1 is chosen when battery % < misc[2] and mode 2 when battery < 5 % (0x5a894).
  - Zone 1 turns red (FF0000, brightness 50) and zone 0 turns off, in blink mode 8.
  - Period 0x4000 ticks (0.5 s, param 500) for mode 1, and 0x8000 (1 s, param 1000) for mode 2.
- **Pairing**: 0x61314(1) sets 0x200024d5 = 1 and starts timer 3 at 0.5 s. `0x5e9dc` drives the two **green** pins (P0.11, P0.04) as plain GPIO (table @0x66948/0x6694a; index 0 would use the blue pins P1.09 / P0.06).
- **LED test** `0x51124`: blue channels full, all others 0.

---

## 7. Firmware version / IDs  [C]
- FW version **0x01010008**, i.e. 1.1.0.8 [H formatting]:
  - RAM 0x20002278 (RW init)
  - bytes `08 00 01 01` in vendor cmd `50 00 00`
  - USB bcdDevice **0x1108**
  - stored in EEPROM 0x0001
- 0x20002274 = 0x01010009 (no direct reference found). 0x2000227c = 0x4707 is the EEPROM layout version.
- Flash 0x50200 = 0x02000965 (returned by `A2 10`). 0x27000 header: app region 0x50000, end 0x6719c, "jump" magic.
- Strings: "Kingston", "HyperX Pulsefire Dart", serial "000000000000", "User 1".

---

## 8. Open questions
1. Physical identity of P1.04 versus P1.02 (Forward/Back), and of LED zone 0 versus zone 1 (logo or wheel). A quick press/observe test answers both.
2. Physical scroll direction that corresponds to A-leads-B (code says it is HID wheel −1).
3. Event 8 (side-button combo) has no consumer that was found. It may be dead code or consumed through a path not traced.
4. Exact behaviour of LED effects 2, 3 and 4, and of zone[1] ("sub" nibble) and zone[3..5].
5. Meaning of cfg[0x00..0x0d], misc[0,1,6..8], the `50 00 01` payload `02 0e 01 05`, and EEPROM 0xFE00/0xFE01.
6. The TWI1 device(s): EEPROM confirmed by access pattern. Is the "Qi receiver" the same chip? (`51 01` reads 16-bit registers from 0x20006614.)
7. Macro event byte format (5 / 10-byte records, 0x1A = long record) and playback timing were not decoded (player not traced).
