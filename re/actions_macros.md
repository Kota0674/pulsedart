# Stock firmware: button actions, keyboard/consumer reports, macros

Source: static analysis of `dump/flash.bin` (app 0x50000–0x68000) and `dump/eeprom.bin`. Listings: `re/tools/out/app.lst`, `re/tools/iul_cmd.lst`; the gap in the SOF handler was re-disassembled with `re/tools/iul_dis.py 0x63d70 0x63e58`.
Tags: **[C]** = CONFIRMED in code or data at the given address. **[H]** = HYPOTHESIS.
Builds on `input_usb_led.md` (button scan, map, USB) and `radio.md` (ESB payloads).

---

## 0. Data flow overview [C]

```
1 ms scan 0x61ab0 ──(debounced edge / wheel detent)──► event FIFO 0x20006194 (128 × 8 B)
                                                          │  one event per report slot
USB:  SOF 0x63d50 every N SOFs ─┐                         ▼
Radio: 1 ms tick 0x5ea44 ───────┴──► 0x60248 report builder ──► 0x58370 dispatcher
                                         │ returns kind 1/2/3 + 7-byte body
                                         ├─ 1 mouse    → USB 0x5ac48 (IF0)    / radio 0x60
                                         ├─ 2 keyboard → 0x5ff64 → USB 0x59bbc (IF2) / radio 0x61
                                         └─ 3 consumer → USB 0x56b7c (IF3)    / radio 0x62
Macro player (app_timer 8, handler 0x62980) ──► pending buffer 0x2000224d + flag 0x2000229d
    USB: sent by the SOF handler (it has priority over the dispatcher's report)
    Radio: sent directly from the timer handler (0x62ae2..0x62b3c)
```

- A "report slot" is every `table@0x66f20[cfg[0x0e]]` = 8/4/2/1 SOFs or 1 ms ticks, i.e. 125…1000 Hz. **Exactly one queued event is consumed per slot.** For example, a 3-detent wheel burst (6 events) takes 6 slots.

---

## 1. Events and the queue

### 1.1 Event record (8 bytes, built on the stack in 0x61ab0) [C]

| Byte | Meaning |
|---|---|
| [0..1] | not written (stack residue), unused |
| [2] | map type (button map byte 0), filled by `0x58988` |
| [3] | map code (map byte 1) |
| [4] | map aux (map byte 2) |
| [5] | **0 = press, 1 = release** (0x61b64 / 0x61b76) |
| [6] | button index 0..5. For the wheel: 6 = count < 0 (map entry 6, code 0xE8) and 7 = count > 0 (map entry 7, code 0xE9) (0x61bdc / 0x61bf8) |
| [7] | not written, unused |

- **Wheel** (0x61bc0..0x61c38): for |count| detents the scan pushes |count| **press + release pairs** (0x61c10..0x61c28). Map entries 6 and 7 are therefore ordinary "buttons" that tap once per detent.
- Events are not queued while `0x200024c3` (combo in progress) is set (0x61b8a / 0x61c0c).

### 1.2 FIFO [C]
- 128 entries × 8 B at **0x20006194**. Write index 0x200025f7 (0x59b60), read index 0x200025f8 (0x59b14).
- When the buffer is full, the oldest entry is dropped (0x59b92: the read index advances).
- `0x59af0` flushes the FIFO (both indexes = 0, buffer zeroed).

### 1.3 Map entry and aux computation [C]
- Map at RAM 0x20007728: 8 entries × (type, code, aux). My EEPROM at 0x0190 holds the factory map (`01 01 01 | 01 02 02 | 01 03 04 | 01 04 08 | 01 05 10 | 07 08 00 | 01 e8 00 | 01 e9 00`).
- Vendor `D4 b t .. c` (0x52306) accepts only **b < 6** (error 1) and **t < 8** (error 2). **The wheel entries 6 and 7 cannot be remapped.** The code value is not validated.
- aux = `0x50c04(t, c)`:
  - t = 1 and c < 6 → `1 << (c−1)`
  - t = 3 → `1 << c`
  - otherwise → c
- The dispatcher uses **aux only for type 2** (the HID key usage). Types 1 and 3 use **code**.

---

## 2. Dispatcher 0x58370 (called only from 0x60248) [C]

Signature: `dispatch(out[8])` → out[0] = report kind (0 none, 1 mouse, 2 keyboard, 3 consumer), out[1..7] = body. 0x60248 pre-sets out[0]=0. The return value is 0 when the queue is empty, 1 otherwise; the caller only looks at out[0].

State:

| RAM | Size | Meaning |
|---|---|---|
| 0x200024c7 | u8 | Mouse button bitmap (HID bits) |
| 0x200024cf | [5] | "Owner" of each mouse button bit = button index that pressed it |
| 0x200024c9 | [6] | Keyboard key array (HID usages, 0 = free) |
| 0x200024c6 | u8 | Button index that owns the active consumer key |
| 0x200024c8 | i8 | Wheel accumulator (read and cleared by 0x602e4) |
| 0x200024c5 | u8 | Count of forced empty reports |
| 0x200024f2 | u8 | Sniper held |
| 0x20002784 | [9] | Status block (§2.8) |
| 0x20008376 | [15] | Macro player (§4.4) |

### 2.1 Prologue (0x5837c..0x58466)
1. **Forced release** (0x5837c):
   - If `c5 ≠ 0`: out[0] = c5, out[1..6] = 0. The FIFO is flushed (0x59af0) and c5 is decremented.
   - The owner array cf[0..5], the key array c9[0..5], c6 and c7 are all cleared.
   - Because c5 starts at 3, the next three slots send an **empty consumer report, then an empty keyboard report, then an empty mouse report**.
   - c5 is set to 3 after button combos (0x583ec / 0x58406) and by 0x200024c3 logic.
2. Dequeue. If the FIFO is empty, return 0.
3. If ev[5] == 0 (any press, including wheel detents): call `0x600b4` = restart the "reactive/trigger" LED effect on zones whose effect is 3. There is no other side effect.
4. **DPI button release after a combo** (ev[6]==5 and ev[5]==1):
   - If 0x200024ea or 0x200024e9 (factory-reset latch) is set, clear that flag, set c5 = 3, clear status[7], and swallow the event.
5. **Status bookkeeping** (0x58414): if not pairing (0x2000223d ≠ 1) and index < 6, then status[8] = 4. Press sets bit `1<<index` in status[7]; release clears it (0x58430..0x58464).

### 2.2 Type 1 - mouse button / wheel (0x5888a) [C]

| code | Press (ev[5]=0) | Release (ev[5]=1) |
|---|---|---|
| 1..5 | `i = code−1`, `cf[i] = index`, `c7 |= bit[i]`, out = {1, c7} | only if `cf[i] == index`: `cf[i] = 0`, `c7 &= ~bit[i]`, out = {1, c7}; otherwise no report |
| 0xE8 | `c8 += 1` (wheel +1), no out kind | nothing |
| 0xE9 | `c8 −= 1` (wheel −1), no out kind | nothing |
| other | nothing | nothing |

- `bit[] = table@0x66953 = 01 02 04 10 08`: code 4 → HID bit 4 (0x10), code 5 → HID bit 3 (0x08).
- "Owner" logic: if two physical buttons are mapped to the same HID bit, only the one that pressed it last can release it.
- For wheel codes the dispatcher returns out[0]=0. 0x60248 then takes the "no event" path, where `0x602e4` puts c8 into the report and clears it. **A button mapped to 0xE8/0xE9 scrolls exactly one step per press. There is no auto-repeat.**

### 2.3 Type 2 - keyboard key (0x58466) [C]
The key is **aux** (ev[4]), which equals the code for type 2.

```
press:   for s in 0..5: if c9[s]==0 or c9[s]==key: break
         if s==6: s=5                       // array full → overwrite slot 5
         if c9[s]==key: no report           // already held
         else c9[s]=key; out[0]=2
release: find s with c9[s]==key; if none → no report
         remove slot s, shift c9[s+1..5] left, c9[5]=0; out[0]=2
both:    out[1..6] = c9[0..5]               // out[7] is NOT written (stack residue)
```

- **Modifiers (0xE0–0xE7) are stored in the key array like any other usage.** They are converted later, and only in slot 0 (§3.1).
- **No firmware auto-repeat.** A held button keeps the key in the report, and the host does typematic repeat. [C] (no timer in this path)

### 2.4 Type 3 - consumer key (0x5851e) [C]
- Press: `c6 = index`, out = {3, lo = table@0x6694c[code & 0xF], hi = 0}.
- Release: only if `index == c6`, out = {3, 0, 0}. Otherwise no report.
- **Only one consumer usage can be active.** A second press replaces the first, and releasing the first button then does nothing.
- Table @0x6694c:

| code | Usage | Name |
|---|---|---|
| 0 | 0xCD | Play/Pause |
| 1 | 0xB7 | Stop |
| 2 | 0xB6 | Scan Previous |
| 3 | 0xB5 | Scan Next |
| 4 | 0xE2 | Mute |
| 5 | 0xEA | Volume Down |
| 6 | 0xE9 | Volume Up |

- Codes 7..15 read past the table into `01 02 04 10 08 02 1d 09 26` (garbage usages). The high byte is always 0, so usages above 0xFF are impossible even though the descriptor allows up to 0x2FF.

### 2.5 Type 4 - macro (0x58564) [C]
Code = macro index. It is not range-checked; code ≥ 6 would read RAM past 0x20007f76. The full behaviour is in §4.5.

### 2.6 Type 7 - special (0x58712) [C]

| code | When | Action |
|---|---|---|
| 6 | **release**, and sniper not held | `0x62e34(0)`: DPI up, no wrap |
| 7 | release, sniper not held | `0x62e34(1)`: DPI down, no wrap |
| 8 | release, sniper not held | `0x62e34(2)`: DPI cycle up, wraps to the first enabled stage |
| 9 | release, sniper not held | `0x62e34(3)`: DPI cycle down, wraps to the last |
| 0x0B | press | f2 = 1. If sniper DPI (0x20007702+8) ≠ current DPI: `0x5eed4(sniper)`. status[5] = 1 |
| 0x0B | release | f2 = 0. If they differ: `0x5eed4(DPI[stage])`. status[5] = 0 |
| other | – | nothing |

- For codes 6..9, after the change: `status[2] = stage` and the LED stage colour is shown (`0x50b10(stage)`), even when the step was a no-op at the end of the range.
- `0x62e34` (0x62e34..0x62edc) works on the enabled-stage list 0x2000250e, with position 0x20002513 and count 0x20002514.
  - It writes the new stage to 0x20007702[0] and programs the sensor (`0x6097c`).
  - It **immediately writes the whole 0x26-byte DPI block to EEPROM 0x0090** (0x62ed6). Every DPI press therefore costs an EEPROM write (about 30 ms blocking).
  - At a no-wrap limit it returns early, with no write.
- No output report is generated for type 7.

### 2.7 Types 0, 5, 6, 8+
Nothing happens and there is no report. [C] Type 0 = disabled [H]. D4 allows 0..7.

### 2.8 Status block 0x20002784 (a by-product of every physical press/release) [C]
- RW default: `ff 03 00 00 00 00 00 00 00 01`.
  - [2] = DPI stage
  - [5] = sniper active
  - [7] = bitmap of held physical buttons 0..5
  - [8] = countdown
- When a report slot has no report and [8] ≠ 0, [8] is decremented. When it reaches 0 (and the mouse is not pairing), the first 8 bytes are sent:
  - USB: `0x5af0c` sends a 64-byte **vendor IN report on IF1** (EP 0x82): `ff 03 st 00 00 sn 00 btn` followed by zeros (0x63e62).
  - Radio: `0x53a5c(0x20002784, 8)` tunnels the bytes as a vendor reply (0x20002324 = 9, 0x20002325 = 1), then [6] = 0 (0x5ec36).
- [H] NGENUITY uses this for its live "button pressed" display.

---

## 3. Report formats

### 3.1 Keyboard [C]
- Descriptor @0x6476c (IF2, EP 0x84): modifier byte (usages E0–E7), 1 reserved byte, 6-key array (0..0xFB). There is no output (LED) report. The report is 8 bytes.
- Pipeline for dispatcher reports:
  1. `0x60248` copies out[1..7] → `r[0..6]` (0x60284).
  2. `0x5ff64` (0x63e24 USB / 0x5ebde radio) converts it:

```
if 0xE0 <= r[0] <= 0xE7:  r[0] = 1 << (r[0]-0xE0)          // slot 0 → modifier bit
else:                     r[1] = r[0]; r[0] = 0             // slot 0 moves to slot 1, OVERWRITING key 2
```

  3. USB `0x59bbc`: report = `[r0, 0x00, r1, r2, r3, r4, r5, r6]`, 8 bytes on IF2.
  4. Radio: `data[2..8] = r[0..6]`, type 0x61.

**Consequences of the stock behaviour** [C]:

| Keys held (in press order) | USB report |
|---|---|
| A | `00 00 A 00 00 00 00 x` |
| A, B | `00 00 A 00 00 00 00 x` - **B is lost** |
| LShift (E1) alone or first | `02 00 k2..k6 x` (correct modifier) |
| A, then LShift | `00 00 A 00 …` - the shift is lost (it was in slot 1) |

- A modifier in slots 1..5 is sent as the array usage 0xE0+n when slot 0 is itself a modifier.
- `x` = out[7] is never written for button-generated keys. It is the top byte of the caller's r3 saved by `push {r2,r3,r4,lr}` at 0x60248, so its value is undefined. [C] that it is unwritten; [H] its value.
- **Recommendation for the new firmware:** build a proper report (modifier bitmap from all E0–E7 in the array, up to 6 non-modifier keys, byte 7 = 0).
- Macro keyboard events bypass `0x5ff64`. They already carry `[mod, k1..k6]` (§4.3).

### 3.2 Consumer [C]
- Descriptor @0x647a8 (IF3, EP 0x85): `05 0c 09 01 a1 01 95 01 75 10 15 01 26 ff 02 19 01 2a ff 02 81 00 c0` = one 16-bit array usage, 1..0x2FF. The report is 2 bytes, LE.
- USB `0x56b7c`: report = u16 out[1..2]. Release = `00 00`.
- Radio 0x62: `data[2..3]` = the same u16 LE. `data[4..8]` hold stale buffer bytes; the builder at 0x5ec02 writes only 2 bytes.

### 3.3 Mouse (for completeness) [C]
- USB `0x5ac48`: 6 bytes `[btn, X lo, X hi, Y lo, Y hi, wheel]`.
- Boot protocol (IF0 instance byte 0x11 == 0) sends 3 bytes: `btn & 7`, X and Y clamped to ±127.
- **The keyboard and consumer senders (0x59bbc, 0x56b7c) and the status sender 0x5af0c check the *mouse* interface's protocol byte (instance 0x647d4 +0x11) and send nothing in boot protocol.**
- Radio 0x60: `data[2]` = btn, `[3..4]` = X, `[5..6]` = Y, `[7]` = wheel.

### 3.4 When release reports are sent [C]
- **Keyboard:** one report per press and one per release, from the dispatcher. There is no report when the key is already held or not found.
- **Consumer:** release = u16 0, sent only by the owning button.
- **Mouse button:** a release report is sent only by the owner.
- **Forced empties:** c5 = 3 → consumer 0, keyboard 0, mouse 0 in 3 consecutive slots (§2.1). Used after the 5 s combos and after the DPI-release swallow.
- **Before a reset or DFU** (0x20002262 = 1, radio only, 0x5eab8): all-zero 0x60, then 0x61, then 0x62 packets.
- **Macro stop:** see §4.5.

### 3.5 Transport selection [C]
- `0x20002469` (VBUS present) selects USB.
- USB path, SOF handler 0x63d70..0x63e92:
  1. Every `interval` SOFs, call 0x60248.
  2. If `0x200026e0 ≠ 0`, drop the slot. [H] 0x200026e0 = the last app_usbd return code (written at 0x60e2c).
  3. **If a macro output is pending (0x2000229d), send the macro report and discard the report 0x60248 just built.** The dispatcher state is kept, so the lost key or button edge shows up in the next report of that kind.
  4. Otherwise send the report by kind.
  5. If there is no report, send the status countdown (§2.8).
- Radio path 0x5ea44:
  - Same order, except that a kind-1 report while a macro is pending is dropped (0x5ebba), and the macro packet was already sent from the timer.
  - The payload is only written when ESB is idle (0x5ecdc, `0x5c28c`). [H] A report built while ESB is busy is lost.

---

## 4. Macros

### 4.1 Storage [C]

| Where | Layout |
|---|---|
| RAM | 0x20007740 + i·0x209, i = 0..5: 9-byte header + 0x200-byte data |
| EEPROM header | 0x0210 + 16·i, 9 bytes (save 0x53cc6, load 0x5129a) |
| EEPROM data | 0x0890 + 0x200·i. Saved in 16-byte pages (0x53d16), loaded in 0x70-byte chunks (0x512c4). **Only `n5·5 + n10·10` bytes are saved or loaded** |

- On load, the rest of the RAM data stays zero (BSS).
- **Collision** [C]: macro 5 data covers 0x1290..0x148F. My EEPROM has a pairing record at **0x1488..0x148F** (`02 02 6e 2f 15 41 01 ff`). A macro-5 program longer than 0x1F8 bytes overlaps the pairing history, and vice versa (pairing writes use 0x1490+8·(n−1), 0x50d62).
- My EEPROM: all six headers are `00 × 9` (empty), and all data areas are 0xFF (never written). Nothing is loaded at boot.

Header (9 bytes):

| Off | Size | Field | Set by |
|---|---|---|---|
| 0 | u16 | `len`. D5 checks ≤ 0x200. **The player uses only the low byte, as an EVENT COUNT** (0x5860a → P[3]) | D5 |
| 2 | u16 | n5 = number of 5-byte events | D6 (recounted) |
| 4 | u16 | n10 = number of 10-byte events | D6 (recounted) |
| 6 | u8 | mode 0..3 | D5 |
| 7 | u16 | repeat count. Only a byte is written (from D5 [7]); forced to 0 when mode ≠ 1 | D5 |

- [H] NGENUITY sends `len` = total event count. If it were a byte count, the player would run past the program into zero bytes, which triggers the delay-0 crash (§4.4). The ≤ 0x200 check suggests the firmware author thought of bytes.
- **For the new firmware, derive the count as n5 + n10, or walk the data.**

### 4.2 Vendor commands [C]
All take `buf[1] = m` (macro index): high nibble must be 0 and low nibble < 6, else error 1.

- **`D5 m 00 xx lenlo lenhi mode rep`** (0x52562):
  - buf[2] must be 0 (else error 2). Error 4 if len > 0x200, mode > 3, or rep > 1000 (a byte, so never).
  - Sets hdr[0..1] = len, hdr[6] = mode, hdr[7..8] = (mode==1 ? rep : 0). **Repeat max is 255.**
- **`D6 m A B events…`** (0x52354): skip `o = (A<<2) | (B>>6)` events, then write `n = B & 0x3F` events.
  - Checks: o < 0x200 and n ≤ 6, else error 1.
  - hdr n5 and n10 are zeroed, then recounted while walking the o existing events (using the stored 0x1A markers).
  - The n new events are copied from buf[4..], at 5 or 10 bytes each (0x1A marker), incrementing n5 or n10.
  - At most 6 events fit (4 + 60 = 64 bytes).
  - **There is no bound check against the 0x200 data size.** A large o overflows into the next macro and beyond. [C]
- **`55 m`** (0x5317a): reply [4..5] = len, [6] = mode, [7..8] = repeat.
  - Bug: index = `buf[1] & 0xF0`, which is always 0 after the check, so **macro 0 is always returned**.
- **`56 m A B`** (0x531c8): same index bug.
  - `o = (A<<4) | (B>>6)` - **shift 4, where D6 uses shift 2** [C]. The offsets only agree when A == 0, i.e. o < 4.
  - n = B & 0x3F ≤ 6, o < 0x200.
  - Walks the events and copies n events into reply[4..].
- `DE 05` saves all 6 macros. `DF AA 05` clears len, n5 and n10 of every macro (0x52692); mode, repeat and data stay.

### 4.3 Event encoding [C]
Byte offsets are relative to the event start. Delays are **milliseconds, u16 LE, the wait after this event** before the next one is processed.

**Keyboard event, 10 bytes** (first byte 0x1A):

| Byte | Meaning |
|---|---|
| 0 | 0x1A (marker; not otherwise used) |
| 1 | HID modifier bitmap (USB report byte 0, radio 0x61 data[2]) |
| 2..7 | 6 key usages (USB report bytes 2..7, radio data[3..8]) |
| 8..9 | delay, ms |

- It is a full **absolute** keyboard state: key down = the key present, key up = a later event without it.
- USB: `0x59bbc(e[1..7])` → `[e1, 0, e2..e7]` (0x63dde). Radio: `data[2..8] = e[1..7]` (0x62afe).
- No `0x5ff64` conversion is applied, so macros **can** send modifiers and 6 keys correctly.

**Mouse event, 5 bytes** (first byte ≠ 0x1A):

| Byte | Meaning |
|---|---|
| 0 | not read by the player (any value ≠ 0x1A) [H: NGENUITY type tag] |
| 1 | mouse button bitmap (HID bits: 0x01 L, 0x02 R, 0x04 M, 0x08 B4, 0x10 B5) |
| 2 | wheel int8 |
| 3..4 | delay, ms |

- Player buffer 0x2000224d = `[btn, 0, 0, 0, 0, wheel]` (0x62a18..0x62a32). X/Y are always 0.
- Radio 0x60: data[2] = btn, [3..6] = 0, [7] = wheel, [8] = 0.
- **USB: only bytes 0..4 are copied** (0x63df8). The wheel byte of the USB report is stale, taken from the report the SOF handler just built and discarded. **Macro wheel does not work over USB.** [C]

The player has no relative-motion or consumer event type. [C]

### 4.4 Playback engine [C]

Player state P = 0x20008376 (15 bytes; zeroed with `0x5047c(P, 15)`):

| Off | Meaning |
|---|---|
| 0 | kind of the last loaded event: 0 none, 1 mouse (5 B), 3 keyboard (10 B) |
| 1 | button index that started it |
| 2 | events played in this pass |
| 3 | events per pass (= hdr[0] low byte) |
| 4 | mode |
| 5..6 | byte offset into data |
| 7..8 | passes completed |
| 9..10 | repeat target |
| 11..14 | pointer to data (hdr + 9) |

Other state:
- 0x2000229d = output pending.
- 0x2000224d = 7-byte output buffer.
- App timer 8 (0x648d4 → 0x20005f88), created **repeated** at 0x5f6d8. Handler 0x61a01 → app_sched → **0x62980**.
- Tick conversion: `ticks = (ms·32768 + 500) / 1000` (0x62a58).
- `0x5f764(8, t)` does nothing if timer 8 is already marked running (0x20002478 bit 7). Every path calls `0x5f81c(8)` (stop) first.

```
timer8_handler():                                   // 0x62980
  if (0x20002323 & 4) return                        // radio vendor-tunnel busy: timer keeps its period, retries
  stop(timer8)
  if P.idx < P.count:
     if pending: delay = 2                          // USB report not yet taken → retry in 2 ms
     else:
        P.idx++ ; e = data + P.off
        if e[0]==0x1A: P.kind=3; out7=e[1..7]; P.off+=10; delay=u16(e[8..9])
        else:          P.kind=1; out=[e[1],0,0,0,0,e[2]]; P.off+=5; delay=u16(e[3..4])
        pending = 1
     start(timer8, ms_to_ticks(delay))
  else:                                             // end of pass
     if mode==1: P.passes++; if P.passes < P.repeat: restart_pass()
     elif mode==2 or mode==3: restart_pass()
     // mode 0 (or repeat reached): stop; P is NOT cleared (kind/idx remain)
  if !USB:                                          // radio: transmit now
     clear 0x20002839[0..15]
     if pending: build 0x61 (kind 3) or 0x60 (kind 1); pending = 0; nrf_esb_write_payload(0x20002834)
restart_pass(): P.idx=0; P.off=0; pending=0; start(timer8, 0x42 ticks ≈ 2.0 ms)
```

- **Delay 0 ms (or anything < 1 ms) reboots the mouse** [C]:
  - 0 ms → 0 ticks.
  - `app_timer_start` (0x54f88) returns error 7 for ticks < 5.
  - `0x5f764` calls `0x54d38` → `0x54cdc`: `cpsid i`, BKPT if a debugger is attached, then **AIRCR SYSRESETREQ**.
  - It also happens when the event count exceeds the stored events, because the zero bytes read as 5-byte events with delay 0.
- Timing summary:
  - First event: **100 ms after the press** (0xccd ticks, 0x58676).
  - Event k+1: delay(k) after event k.
  - Between passes: delay(last) + 2 ms.
- USB: a pending event is sent at the next report slot (≤ 1 ms at 1000 Hz). While pending, the macro report replaces whatever the dispatcher produced in that slot.

Modes [C] (names are [H]):

| mode | Name | Loop behaviour |
|---|---|---|
| 0 | once | one pass |
| 1 | repeat N | N passes (N = hdr[7], 0 or 1 → 1 pass, max 255) |
| 2 | toggle | loops forever until stopped by a macro button press |
| 3 | while held | loops forever until the starting button is released |

### 4.5 Type-4 press/release handling (dispatcher 0x5856c..0x58710) [C]

**Press** of a macro button (any macro index):
```
stop(timer8)
if P.mode == 2:                                     // mode of the macro CURRENTLY loaded
   pending = 0; P.off = 0
   if P.kind != 0:                                  // it is running → stop it (toggle off)
      if P.kind==1: out = mouse report, buttons 0   // releases ALL mouse buttons on host
      if P.kind==3: out = keyboard report all 0
      memset(P,0,15); return
start(index = code):
   pending=0; P.idx=0; P.off=0; P.passes=0
   P.mode=hdr[6]; P.count=hdr[0]; P.btn=ev[6]; P.repeat=hdr[7..8]; P.ptr=data
   P.kind = (data[0]==0x1A) ? 3 : 1
   if P.kind==1: clear owner array 0x200024cf[0..5]      // (see bug below)
   if P.kind==3: out = keyboard report all 0             // release keys before starting
   P.kind = 0; start(timer8, 0xccd)                       // first event after 100 ms
```

**Release** of a macro button:
- If `P.mode == 3 && P.btn == index`: stop the timer, pending = 0, P.off = 0.
  - kind 1: clear the owner array. kind 3: out = empty keyboard report.
  - Then `memset(P, 0, 15)`.
- Any other release does nothing.

Consequences [C]:
- Pressing any macro button while a mode 0/1/3 macro runs **restarts** playback, with the new macro if the index differs. The previous one is abandoned without a release report, except for the keyboard-zero report sent when its first event is keyboard.
- Mode 2 stops only if the running macro's P.kind ≠ 0. During the first 100 ms after start P.kind == 0, so a second press in that window restarts instead of stopping.
- Releasing a mode-0/1/2 macro button mid-macro has no effect; playback continues.
- A mode-3 stop sends a keyboard-zero report for keyboard macros. For mouse macros it only clears the owner array and sends **no** mouse release. A button the macro left pressed stays pressed on the host until the next mouse report, which carries the physical button state 0x20002515.
- **Stock bug:** starting a macro whose first event is a mouse event clears `0x200024cf`. A physical mouse button held at that moment can then no longer release its HID bit (§2.2 owner check), so it stays stuck until pressed again.
- Macro output does not update c7 or c9. After a macro, the next dispatcher report returns the host to the physical state.

---

## 5. Where output goes: wireless vs USB (summary) [C]

| Source | USB (VBUS present) | Wireless |
|---|---|---|
| Button type 1 / wheel / motion | IF0 6-byte report (0x5ac48) | 0x60, data[2..7] |
| Button type 2 | 0x5ff64 → IF2 8-byte report (0x59bbc) | 0x5ff64 → **0x61**, data[2..8] = 7 bytes (`[mod or 0, key…, x]`) |
| Button type 3 | IF3 u16 (0x56b7c) | **0x62**, data[2..3] = u16 LE |
| Macro keyboard event | IF2 `[e1,0,e2..e7]` from the SOF handler | 0x61, data[2..8] = e[1..7], sent from the timer handler |
| Macro mouse event | IF0 `[btn,0,0,0,0,stale]` | 0x60, data[2]=btn, [7]=wheel |
| Status 0x20002784 | IF1 vendor IN, 64 B | vendor tunnel (0x53a5c) |

- Packet buffer: 0x20002834 = `nrf_esb_payload_t` (len@0 = 9, pipe@1 = 2, data@5). `data[0]` = retransmit counter.
- The dongle forwards data[2..8] of 0x60/0x61/0x62 to the SONiX as report types 0/1/2 (`dongle_radio.md`).
- [H] The dongle side rebuilds the 8-byte keyboard report as `[data2, 0, data3..data8]`, mirroring 0x59bbc.

---

## 6. Implementation notes for the new firmware
1. Keep the event model: press/release per map entry, and one wheel tap (press + release) per detent on entries 6/7.
2. Keyboard: fix the report. Build the modifier byte from every held E0–E7 key, list up to 6 other keys, and set byte 7 = 0. For dongle compatibility, send 0x61 as `data[2]=mod, data[3..8]=keys`.
3. Consumer: 16-bit usage. To be compatible with stock NGENUITY maps, keep table 0x6694c for codes 0..6.
4. Macros:
   - Accept the D5/D6 formats above.
   - Compute the event count from n5 + n10 (or walk the data), not from `len`.
   - Clamp delay to ≥ 1 ms.
   - Bound-check D6.
   - Fix the 55/56 index and offset bugs.
   - Send the macro wheel over USB.
   - Release everything the macro pressed when it is aborted.
5. DPI actions trigger on release. Sniper (0x0B) works while held and blocks DPI steps.
6. The DPI stage is saved to EEPROM on every change (stock). Consider deferring the write.

---

## 7. Open questions
1. The exact value of the unwritten report byte `x` (out[7]) in keyboard reports built from buttons. It is stack residue and depends on the caller's r3.
2. Whether NGENUITY's `len` (D5 [4..5]) is an event count (the player treats it as one) or a byte count. Capturing one NGENUITY macro upload would settle it.
3. The meaning of byte 0 of a 5-byte event (ignored by the player). It may carry an event type (e.g. button/wheel/delay-only) that NGENUITY uses for display.
4. The dongle/SONiX handling of 0x61 and 0x62 (how data[2..8] map to its USB reports).
5. Where 0x200024ea is set: no direct literal store was found, so it may be reached through address arithmetic.
6. 0x200026e0 (USB skip condition) and 0x20002323 bit 2 (radio tunnel busy; it gates both reports and the macro timer): the exact semantics were not traced.
