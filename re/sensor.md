# PMW3389 sensor driver - stock firmware (static analysis)

Source: `dump/flash.bin`, main app 0x50000–0x68000. All addresses are flash addresses in the app
unless noted as RAM (0x2000xxxx). Default RAM values come from the decompressed RW-init image
`re/ram_init.bin` (built by `re/tools/scatter.py`).
Helper scripts used: `re/tools/sensdis.py` (disassembler with literal annotation),
`re/tools/blrefs.py` (BL caller search).

Status tags: **[C]** = CONFIRMED (seen in code/data), **[H]** = HYPOTHESIS.

---

## 1. Low-level SPI layer

| Item | Value | Where | Tag |
|---|---|---|---|
| Peripheral | SPI0 (legacy SPI, 0x40003000) via Nordic `nrf_spi_mngr` (instance @0x6469c, config @0x646b4) | 0x5f408, 0x5d3d8, 0x5d414, 0x5d474 | C |
| Config bytes @0x646b4 | `16 11 14 ff 07 ff 00 00 00 00 00 20 03 00` = SCK P0.22, MOSI P0.17, MISO P0.20, SS none, IRQ prio 7, ORC 0xFF, FREQUENCY 0x20000000 (2 MHz), mode 3, MSB first | 0x646b4 | C |
| NCS | P0.13, software GPIO (clear = 0x5cb0c, set = 0x5ce1a) | everywhere | C |
| Blocking transfer | `nrf_spi_mngr_perform(mngr, cfg, xfer, 1, 0x6124d)`: TX-only wrapper 0x5f598(buf,len), RX-only wrapper 0x5f4c4(buf,len) | 0x5f598 / 0x5f4c4 | C |
| Async transaction queue | 0x5f500(desc) → `nrf_spi_mngr_schedule`; desc = `{u8 slot; u8* tx; u8 txlen; u8* rx; u8 rxlen; begin_cb; end_cb}`, 9 slots, txlen+rxlen ≤ 8 | 0x5f500 | C |
| SPI de-init (before sleep) | wait up to 1000 polls for NCS high, force NCS high, save state, `nrf_spi_mngr_uninit`, SCK/MOSI/MISO pins → default (disconnected) | 0x6181c → 0x5f5d4 | C |
| SPI re-init (after wake) | `0x5f408` again, restore driver state; **sensor is NOT re-initialised** | 0x60728 | C |

Delays: `delay_us(n)` = 0x5da90 (calls cycle loop 0x64920 with n*64 cycles @64 MHz) **[C]**;
`delay_ms(n)` = 0x5b150 (n × delay_us(1000)) **[C]**.

### Register access primitives (timing exactly as implemented)

**Write register** `sensor_write(addr, val)` @0x5fe0e **[C]**
```
NCS low; delay_us(1)
SPI TX [addr|0x80, val]
delay_us(35)          ; tSCLK-NCS (write)
NCS high
delay_us(180)         ; tSWW / tSWR
```

**Read register** `sensor_read(addr, *out)` @0x5fd52 **[C]**
```
NCS low; delay_us(1)
SPI TX [addr]          ; bit7 = 0
delay_us(160)         ; tSRAD
SPI RX 1 byte
delay_us(1)
NCS high
delay_us(20)          ; tSRW / tSRR
```

Callback helpers used by async transactions (referenced only through pointers) **[C]**:

| Addr | Action | Used as |
|---|---|---|
| 0x5fd2c | NCS low (no delay) | begin_cb of the first transfer of a transaction |
| 0x56bac | delay_us(10) | begin_cb of motion-burst RX (tSRAD-MOTBR as implemented) |
| 0x5fd18 | NCS high; delay_us(20) | end_cb of motion-burst RX |
| 0x5fd38 | delay_us(35); NCS high; delay_us(180) | end_cb of async register write |

---

## 2. Init sequence - `sensor_init()` @0x5fbd0 (called once from boot at 0x5a2ac)

Driver state variable `0x200024a0`: 0 = off, 1 = SPI up, 2 = product ID OK, 3 = running **[C]**.
Motion buffer `0x200024a4` (7 bytes) cleared **[C]**.

| # | Action | Value / timing | Addr | Tag |
|---|---|---|---|---|
| 1 | state = 0, clear motion buffer | | 0x5fbd2 | C |
| 2 | P0.13 (NCS): cfg output, set high | | 0x5fbe2–0x5fbea | C |
| 3 | P1.07: cfg output, set high (**never cleared anywhere**; other `0x27` constants in the app are USBD IRQn 39 in NVIC code, not this pin) | | 0x5fbee–0x5fbf6 | C |
| 4 | SPI init (`nrf_spi_mngr_init`); abort if error | no delay between P1.07 high and first SPI access | 0x5fbfa | C |
| 5 | state = 1 | | 0x5fc02 | C |
| 6 | **Write 0x3A (Power_Up_Reset) = 0x5A** | | 0x5fc0c | C |
| 7 | delay 50 ms | | 0x5fc12 | C |
| 8 | Read 0x00 (Product_ID); **abort unless 0x47** (state stays 1) | | 0x5fc1a–0x5fc26 | C |
| 9 | state = 2 | | 0x5fc28 | C |
| 10 | Read 0x02, 0x03, 0x04, 0x05, 0x06 (values discarded) | each with read timing above | 0x5fc30–0x5fc50 | C |
| 11 | SROM download + SROM_ID check + 0x3D handshake (`0x5fd8c`, see §3); abort if it returns 0 (state stays 2) | | 0x5fc54 | C |
| 12 | state = 3 | | 0x5fc5a | C |
| 13 | **Write 0x0F (Resolution_H) = cpi_val >> 8** | cpi_val = DPI table[cur_idx], see §5 | 0x5fc70 | C |
| 14 | **Write 0x0E (Resolution_L) = cpi_val & 0xFF** | | 0x5fc80 | C |
| 15 | rebuild enabled-DPI-step list (0x50aa4) | no SPI | 0x5fc84 | C |
| 16 | **Write 0x50 (Motion_Burst) = 0x01** (arm burst mode) | | 0x5fc8c | C |

### 3. SROM download - `0x5fca0`, wrapped by retry/check `0x5fd8c`

Retry loop (0x5fd8c), max **3 attempts** **[C]**:

| # | Action | Value / timing | Addr | Tag |
|---|---|---|---|---|
| a | **Write 0x10 (Config2) = 0x00** (Rest_En = 0) | | 0x5fca6 | C |
| b | **Write 0x13 (SROM_Enable) = 0x1D** | | 0x5fcae | C |
| c | delay 10 ms | | 0x5fcb4 | C |
| d | **Write 0x13 (SROM_Enable) = 0x18** | (write primitive already adds 180 µs) | 0x5fcbc | C |
| e | NCS low; delay 1 µs | | 0x5fcc2 | C |
| f | TX 0xE2 (0x62 SROM_Load_Burst \| 0x80) | | 0x5fcd4 | C |
| g | delay 140 µs | | 0x5fcda | C |
| h | for i in 0..4093: TX srom[i]; delay 30 µs | length = u16 @0x65924 = 0x0FFE | 0x5fce2–0x5fd00 | C |
| i | NCS high; delay 200 µs | | 0x5fd02–0x5fd0a | C |
| j | Read 0x2A (SROM_ID) | | 0x5fda8 | C |
| k | feed WDT (0x5fb98 → WDT RR = 0x6E524635) | | 0x5fdac | C |
| l | if SROM_ID == 0x05 **or SROM_ID == 0x00** → continue; else retry (after 3 failures return 0) | accepting 0x00 looks like a firmware bug / lenient check | 0x5fdb0–0x5fdbe | C |
| m | **Write 0x3D = 0x80** | | 0x5fdca | C |
| n | delay 675 µs (0x2A3) | | 0x5fdd2 | C |
| o | poll: read 0x3D, delay 625 µs (0x271), until value == **0xC0**, max 55 reads (no error if it times out) | | 0x5fdd8–0x5fdf6 | C |
| p | **Write 0x3D = 0x00** | | 0x5fdfe | C |
| q | **Write 0x50 (Motion_Burst) = 0x01** | | 0x5fe06 | C |
| r | return 1 | | 0x5fe0a | C |

Register 0x3D is not in the public PMW3389 register list I know of; the write-0x80 / wait-for-0xC0 /
write-0x00 pattern looks like a PixArt-provided calibration or "init complete" handshake **[H]**.
Copy it unchanged into a new driver.

**NOT done anywhere in the firmware [C]** (checked every caller of the write primitive 0x5fe0e, of the
async queue 0x5f500 and of the raw SPI helpers 0x5f598/0x5f4c4; that covers every SPI0 access):
- no write to Config2 = 0x20 after download → **Rest modes stay disabled** (Config2 = 0x00),
- no Config5 (0x2F/0x2E, separate Y CPI), Angle_Tune (0x11), Angle_Snap (0x42/0x43),
  Lift_Config (0x63), LiftCutoff_Cal*, Run_Downshift/Rest*_Rate/Rest*_Downshift (0x14–0x1C),
  Min_SQ_Run, Ripple_Control, Control, Shutdown (0x3B) writes.
  All of these stay at post-SROM defaults (default lift-off distance, no angle snapping,
  X/Y CPI taken from Resolution only).

Full list of register writes the firmware ever issues **[C]**:

| Reg | Values | Where |
|---|---|---|
| 0x3A Power_Up_Reset | 0x5A | init 0x5fc0c |
| 0x10 Config2 | 0x00 | SROM download 0x5fca6; also 2× in the vendor "factory reset" command (0x51ea8) |
| 0x13 SROM_Enable | 0x1D, 0x18 | SROM download |
| 0x62 SROM_Load_Burst | 4094-byte burst | SROM download |
| 0x3D (undocumented) | 0x80, then 0x00 | after SROM_ID check |
| 0x50 Motion_Burst | 0x01 | end of SROM routine, end of init, and after every debug register read (0x539c2) |
| 0x0F Resolution_H / 0x0E Resolution_L | CPI value (H first, then L) | init (blocking) and DPI changes (async, 0x6097c) |

Register reads: 0x00, 0x02–0x06, 0x2A, 0x3D (init); 0x02 (pre-sleep); 0x50 burst (motion);
single read of 0x50 as a watchdog/poke (0x5abac); any register through a vendor debug command (§8).

---

## 4. SROM blob

| Item | Value | Tag |
|---|---|---|
| Address | **0x64926** (pointer literal @0x5fd10; the byte after the `delay` code at 0x64920) | C |
| Length | **0x0FFE = 4094 bytes**, u16 stored right after the blob @0x65924 (literal @0x5fd14) | C |
| First bytes | `01 05 8d 92 63 60 1e be fe 5f 1d b8 f2 66 4e ff` | C |
| Last bytes | `… 0e 9e 9f bc fa 57 2c bb d5 09 71 60 31 94 f7 2b` | C |
| SROM version | byte[1] = **0x05**, the same value the code accepts from SROM_ID (0x2A) | C |
| Extracted | `re/srom_pmw3389.bin` (local, not published) | C |
| sha256 | `e1848d529531e0e8048925b294318441eec0e1f14b85284d643293a68ff34202` | C |

Compared with the SROMs floating around publicly it matches **neither**: the PMW3360 one starts
`01 04 …` (ID 0x04), the PMW3389 one in mrjohnk's Arduino repo starts `01 e8 …` (ID 0xE8) **[H]**, not
compared byte for byte. This one starts `01 05` (ID 0x05), so it is a different
(HyperX/PixArt-supplied) revision.

**Second, unreferenced SROM-like blob [C]:** 0x6592A–0x66927, 4094 bytes, followed by its own u16
0x0FFE @0x66928; starts `01 02 84 8e 5b a1 fe 7e 7e 5f 1d b8 f2 66 4e ff` (byte[1] = 0x02). The 4 bytes
before it (0x65926) are `01 81 02 00` (meaning unknown). No code or literal references it (only
0x64926/0x65924 are referenced), so it is dead data: probably an older SROM revision (ID 0x02) left
linked in **[H]**. It differs from the active blob in 4075 of 4094 bytes. Extracted for reference to
`re/srom_pmw3389_alt_id02.bin`, sha256
`b152fca17c2d5742587e98cc7c3022a3852b210a70610ddfcab16e823defff92`. Use the ID 0x05 blob.

---

## 5. Motion reading

### Burst read - `sensor_motion_poll(out)` @0x6030c (called from the report builder 0x60248)
Pipelined: each call returns the result of the **previous** burst and queues the next one **[C]**.

1. Return nothing if `0x2000249c != 0` or state != 3 **[C]**.
2. Consume the previous result from `0x200024a4` **[C]**:
   - `buf[0]` = Motion; if bit7 (MOT) is set: `*(u32*)out = *(u32*)&buf[2]` = **Delta_X_L, Delta_X_H,
     Delta_Y_L, Delta_Y_H** (raw little-endian int16 X then int16 Y, copied without scaling/swap);
     `buf[6]` (SQUAL) → `0x200024b0`; return 1.
     If all four delta bytes are 0 → return 0 and set flag `0x2000224c = 1`.
   - if MOT = 0: out = 0.
   - Clear the buffer.
3. **If MOTION pin P0.15 reads high (no motion) → do not queue a new burst** (0x6037a,
   `gpio_read(15)`). Also skip if `0x20002330 == 0` **[C]**.
4. Reload idle counter `0x200022ac = 5000` **[C]**.
5. Queue transaction slot 0: TX `[0x50]` (1 byte), begin_cb = NCS low **[C]**.
6. Queue transaction slot 1: RX **7 bytes** → `0x200024a4`, begin_cb = delay 10 µs, end_cb = NCS high + 20 µs **[C]**.

Burst format as used (7 bytes) **[C]** (field names from the PMW3389 datasheet):

| Byte | Content |
|---|---|
| 0 | Motion (bit7 = MOT) |
| 1 | Observation (ignored) |
| 2,3 | Delta_X_L, Delta_X_H (int16) |
| 4,5 | Delta_Y_L, Delta_Y_H (int16) |
| 6 | SQUAL (saved to 0x200024b0, no other use found) |

Notes:
- The burst is never re-armed by writing 0x50 before each read. It is armed once (write 0x50 = 0x01 at
  init) and re-armed only after other register accesses (debug read path 0x539ac) **[C]**.
- The firmware waits only 10 µs + scheduling overhead between the 0x50 address byte and the data. The
  datasheet tSRAD_MOTBR is 35 µs (from memory) **[H]**. A new driver should use ≥35 µs.
- The report builder 0x60248 puts buttons at [0], X/Y int16 at [1..4] and wheel at [5] (via 0x602e4) **[C]**.

### MOTION pin (P0.15)
- **Running: polled as a level** (low = motion pending) before each burst, via GPIO IN read.
  No GPIOTE event is used for it **[C]** (0x6037a).
- **Before sleep** (0x56d44 / 0x56e48): P0.15 is set to input with pull-up; then `while (P0.15 == 0)`
  it reads register 0x02 (Motion) to clear pending motion; then SPI is de-initialised; then P0.15 (and
  P1.06, 0x26) are set to **pull-up + SENSE_LOW** (0x5caa2(pin,3,3)) as wake sources **[C]**
  (0x56d82–0x56d92). Motion wakes the MCU.
- **Idle poke:** in the main loop (0x5aba2), when `0x200022ac` reaches 0 (it is reloaded to 5000 on
  every queued burst), a blocking single read of 0x50 runs and 0x50 = 0x01 is written again **[C]**.
  The counter unit is probably 1 ms, so about 5 s **[H]**.

---

## 6. DPI / CPI

CPI register encoding **[C]**: a 16-bit value `v` goes to Resolution_H (0x0F) = v>>8, then Resolution_L (0x0E) = v&0xFF.
Host commands limit `v` to **2..0x140 (320)** (0x52244–0x5224e, 0x522de–0x522e8).
With the PMW3389 rule CPI = v × 50, that is 100…16000 CPI in 50-CPI steps **[H - the formula is from the datasheet; the limits are C]**.

### DPI profile struct in RAM `0x20007702` (0x26 bytes, stored under settings key 0x90) **[C]**

| Off | Size | Meaning | Default (from RW-init @0x200027cb) |
|---|---|---|---|
| +0x00 | u8 | current DPI step index (0..4) | 0 |
| +0x01 | u8 | ? | 1 |
| +0x02 | u8 | number of enabled steps | 3 |
| +0x03 | u16 (unaligned) | min CPI value reported to host | 0x0002 |
| +0x05 | u16 (unaligned) | max CPI value reported to host | 0x0140 |
| +0x07 | u8 | enabled-step bitmask (5 bits, ≤0x1F) | 0x07 |
| +0x08 | u16 | "DPI shift/sniper" CPI value | 16 (800 CPI) |
| +0x0A | u16[5] | CPI value per step | 16, 32, 64, 128, 320 |
| +0x14 | 3 | ? (unused 0) | 00 00 00 |
| +0x17 | u8[5][3] | DPI indicator colour per step (R,G,B) | see below |

**Default DPI table [C] (CPI values from ×50 [H]):**

| Step | Reg value | CPI | Enabled by default | Colour bytes @+0x17 | Colour, if bytes are R,G,B [H] |
|---|---|---|---|---|---|
| 0 | 0x0010 | 800 | yes (**default step**) | 00 00 FF | blue |
| 1 | 0x0020 | 1600 | yes | FF FF 00 | yellow |
| 2 | 0x0040 | 3200 | yes | 00 FF 00 | green |
| 3 | 0x0080 | 6400 | no | FF 00 00 | red |
| 4 | 0x0140 | 16000 | no | FF C0 CB | pink |

Defaults are copied from RW-init data `0x200027cb` on factory reset (vendor command, 0x5265a/0x526e6),
and then loaded from settings storage key 0x90 at boot (0x5a100, via the `0x50c40` storage accessor,
**not** the internal flash page 0xEF000, which holds only 7 bytes) **[C]**. The user's live values may
therefore differ from these defaults.

Enabled-step list **[C]**: `0x50aa4` builds `0x2000250e[]` (indices of set bits in the mask),
count in `0x20002514`, and the position of the current index in `0x20002513`.

### Changing DPI
- `sensor_set_cpi(v)` @0x6097c: only if state ≥ 3; queues two async writes: `[0x8F, v>>8]` (slot 2)
  then `[0x8E, v&0xFF]` (slot 3), each with begin = NCS low and end = 35 µs / NCS high / 180 µs **[C]**.
  Both use the same 2-byte buffer 0x200024b1. 0x5f500 memcpy's the TX bytes to 0x20002405
  (0x5f518), but the slot keeps the **original** pointer. The second schedule may therefore overwrite
  the first transaction's data before it is clocked out, which is a possible race/bug in the stock
  firmware **[H]**. A new driver should use separate buffers or blocking writes.
- `dpi_step(mode)` @0x62e34, then apply + save to settings key 0x90 **[C]**:

| mode | Action code (button function, 0x58712…) | Behaviour |
|---|---|---|
| 0 | 7/6 | next step, stop at last |
| 1 | 7/7 | previous step, stop at first |
| 2 | 7/8 | next step, **wrap to first** (cycle) |
| 3 | 7/9 | previous step, wrap to last |
| - | 7/0x0B | DPI shift: on press apply +0x08 value (default 800 CPI), on release restore the current step (0x58816–0x58886) |

- Default button map (RW-init 0x200027f1 → RAM 0x20007728, 3 bytes per button):
  `01 01 01 | 01 02 02 | 01 03 04 | 01 04 08 | 01 05 10 | 07 08 00 | 01 e8 00 | 01 e9 00`.
  The 6th entry `07 08` = function "DPI cycle up with wrap" → **the DPI button cycles 800 → 1600 → 3200 → 800** **[H for the button↔entry mapping; C for the table bytes and the code-8 behaviour]**.
- After a change, 0x50b10(step) starts an LED effect (mode 7) on **both** LED zones using the step's
  colour from +0x17, parameter 0x32, and runs timers 4/5 for 0x3D7 (983) ticks, probably ms **[C for the code; H for the timing unit]**.
- Vendor HID commands that touch DPI (handler around 0x52130–0x522f0) **[C]**: sub-cmd 0 = select step
  (`[4]` < 5 and enabled), 1 = set enabled mask (`[4]` ≤ 0x1F, resets to the first enabled step),
  2 = set step value (`[2]` = step, u16 `[4]` in 2..320), 3 = set step colour (3 bytes),
  4 = set shift value (u16 in 2..320). Framing and report IDs are for the USB/HID report.

---

## 7. Lift-off, angle snapping, polling rate, power

| Topic | Finding | Tag |
|---|---|---|
| Lift-off distance | No Lift_Config (0x63) or LiftCutoff_* writes. Sensor runs at the SROM default (datasheet default is 2 mm [H]). | C (no write) |
| Angle snapping | No 0x42/0x43 writes, so off (default). | C (no write) |
| Angle tune | No 0x11 write. | C |
| Rest modes | Config2 forced to 0x00 before SROM download and never set to 0x20 → **Rest1/2/3 disabled**; no rest-rate or downshift registers are written. | C |
| Shutdown | 0x3B never written; P1.07 never driven low. The sensor stays powered and in Run mode while the MCU sleeps (only SPI is de-initialised, and MOTION wakes the MCU). | C (absence) / H (power consequence) |
| Wake | Sensor is not re-initialised on wake (0x5fad4 → 0x60728 only re-inits SPI). | C |
| Polling rate | Report divider = table `0x66f20` = `{8, 4, 2, 1}` indexed by `0x200076c4[0x0E]` (default 3 → divider 1) (0x589ac). The report is generated when a tick counter reaches the divider (0x5eb52, 0x63d82). On a 1 kHz tick that gives 125/250/500/1000 Hz, default 1000 Hz. | C (table/default) / H (1 kHz base tick) |
| Sensor frame rate / run mode | Nothing configured. The sensor free-runs; the MCU reads it only when MOTION is low. | C |

---

## 8. Other sensor-related code

- **Vendor debug read** (0x530ea): HID command bytes `[1] = 0x94, [2] = 0x87` → `sensor_read(reg = [4])`,
  reply in `[5]`. Implemented by 0x539ac, which re-arms burst (write 0x50 = 0x01) unless reg == 0x50 **[C]**.
- **Factory reset** (0x51df2): command `[1..5] = AA 55 D0 AA 55` resets both 13-byte LED records at
  0x200076e8 and writes Config2 = 0x00 once per record (2×) **[C]**.
- The sensor-related wake callback 0x5fad4 is registered with 0x5efc0 before sleep **[C]**.

---

## 9. Driver port - condensed sequence

```
// Pins: SCK P0.22, MOSI P0.17, MISO P0.20, NCS P0.13 (GPIO), MOTION P0.15 (in, pull-up), P1.07 out=1
// SPI mode 3, 2 MHz, MSB first
gpio_out(P0_13, 1);  gpio_out(P1_07, 1);
spi_init();
wr(0x3A, 0x5A);                 delay_ms(50);
if (rd(0x00) != 0x47) fail;
rd(0x02); rd(0x03); rd(0x04); rd(0x05); rd(0x06);
for (try = 0; try < 3; try++) {
    wr(0x10, 0x00);
    wr(0x13, 0x1D);             delay_ms(10);
    wr(0x13, 0x18);
    ncs(0); delay_us(1); spi_tx(0xE2); delay_us(140);   // stock uses 140 µs
    for (i = 0; i < 4094; i++) { spi_tx(srom[i]); delay_us(30); }
    ncs(1); delay_us(200);
    id = rd(0x2A);
    if (id == 0x05 /* stock also accepts 0x00 */) break;
}
wr(0x3D, 0x80); delay_us(675);
for (n = 0; n < 55 && rd(0x3D) != 0xC0; n++) delay_us(625);
wr(0x3D, 0x00);
wr(0x50, 0x01);
wr(0x0F, cpi >> 8); wr(0x0E, cpi & 0xFF);  // cpi = CPI/50, default 16 (800 CPI)
wr(0x50, 0x01);
// optional, not in stock: wr(0x10, 0x20) to enable rest modes for battery life

// wr(): ncs0, 1us, [a|0x80, v], 35us, ncs1, 180us
// rd(): ncs0, 1us, [a], 160us, rx1, 1us, ncs1, 20us
// motion: if (!gpio_in(P0_15)) { ncs0; tx 0x50; wait >=35us (stock 10us); rx 7; ncs1; 20us }
```

---

## 10. Open questions

1. **P1.07 purpose**: it is driven high once, before the first SPI access, and never low. It is
   probably sensor power (LDO/load switch enable) or NRESET. Check on the PCB. No settle delay is
   used between P1.07 high and Power_Up_Reset.
2. **Register 0x3D handshake** (0x80 → wait 0xC0 → 0x00) is undocumented in public material I know of.
   Keep it as is.
3. The SROM_ID check accepts 0x00 as success (0x5fdbc). Check whether that is deliberate (for example
   tolerating a read glitch) or a bug. A new driver should require 0x05.
4. Rest mode is disabled (Config2 = 0x00). Measure whether sleep current with the stock firmware
   confirms the sensor stays in Run mode, and whether a replacement should use Config2 = 0x20.
5. Settings storage behind `0x50c40` (keys 0x10, 0x20, 0x60, 0x90 DPI, 0x110 LED, 0x190 buttons, 0x290
   …) is not internal flash (page 0xEF000 has only 7 bytes). Possibly an I²C EEPROM or another device
   on TWI1, left for the storage/TWI analysis. The user's live DPI table may differ from the defaults above.
6. Units of the 5000-count idle poke counter (0x200022ac) and of the DPI-LED timer (0x3D7), and the
   base tick behind the polling divider: find the timer/RTC that decrements them.
7. Colour byte order of the DPI indicator (+0x17 triplets) must be checked against the PWM channel map
   (0x646c4) in the LED analysis.
8. Downstream X/Y processing: 0x6030c copies raw sensor deltas. Axis inversion/rotation, if any,
   happens later in the report/radio path (not checked here).
9. The 4 bytes `01 81 02 00` @0x65926 and the unused ID 0x02 SROM @0x6592A: purpose unknown (dead data).
