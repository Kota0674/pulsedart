# Boot chain, flash layout, firmware update, persistent settings

Source: static analysis of `dump/flash.bin` (sha256 ed01cdf4…030e1a3f), `uicr.bin`, `ficr.bin`.
**C** = CONFIRMED (seen in code/data), **H** = HYPOTHESIS.

Tools written for this (all in `re/tools/`):
- `rdis.py START END VECTORS` – recursive-descent Thumb disassembler with prologue scan. Listings:
  `re/tools/out/bl.lst` (bootloader 0x28000–0x327b0), `re/tools/out/app.lst` (app 0x50000–0x6718c).
- `blfind.py START END target…` – brute-force BL/B.W finder (catches calls the linear listings miss).
- `scatter.py` – decodes Region$$Table of all three images, implements armlink `__decompress`,
  writes `re/ram_init.bin` (app) + `re/tools/out/ram_init_bootloader.bin`, `ram_init_stub.bin`.

---

## 1. Flash map (C)

| Range | Size | Content |
|---|---|---|
| 0x00000–0x004f0 | 1.2 KB | **Boot stub / IRQ forwarder** (own vector table, SP 0x20004218, reset 0x24d) |
| 0x00500–0x26fff | | erased |
| 0x27000–0x2701f | 1 page | **Boot/DFU header page** (8 words, see §3) |
| 0x28000–0x327af | ~42 KB | **Bootloader** = full second image: vectors SP 0x20006430, reset 0x283e1. USB-HID DFU, Nordic SDK (`app_usbd`, `app_timer`, `nrf_fstorage`), Primax sources (`main_ptx_with_usb.c`, `primax_usb.c`, `inner_flash_controller.c`, `primax_apptimer_mgr.c`) |
| 0x50000–0x6718b | 94 604 B | **Application** (SP 0x2000a510, reset 0x503c5) |
| 0x6718c–0x6719b | 16 B | 0xFF padding covered by the header CRC (image padded to a multiple of 60 bytes: 0x1719c = 60×1577) |
| 0xED000–0xEEFFF | | bootloader `nrf_fstorage` range (never written by code found) |
| 0xEE000 | page | app writes 2 bytes here (mode byte), no reader found (§5.3) |
| 0xEF000 | page | **Pairing record** (8 bytes, §5.1) |
| 0xF0000–0xFFFFF | | erased |

External **I²C EEPROM at 7-bit address 0x50** on TWI1 (P0.00/P0.01) holds user settings (§5.2). (C: address 0x50 in
TWI context struct at RAM 0x2000247c, used by the 16-bit-address read/write routine 0x50c40.)

---

## 2. Boot stub 0x00000–0x01000 (C)

Vector table at 0x0: SP 0x20004218, Reset 0x24d, **every** exception/IRQ entry = 0x221 (forwarder), except the
reserved slots and IRQ 30, 31, 43, 44, 46 (reserved on nRF52840) which are 0.

Reset path:
1. `0x24c` Reset_Handler → `blx SystemInit` (0x295) → `bx __main` (0x201).
2. **SystemInit 0x294** = Nordic `system_nrf52840.c`: errata writes (FICR 0x10000130/134 variant check;
   0x4000010c/110=0, 0x40000538=0, copy FICR 0x10000404.. → TEMP 0x4000C520.. (errata 66), 0x4000568c=0x38148,
   0x4000f518=0xfb, 0x40000ee4 bfi from FICR 0x10000258, 0x40029640=0x200, RESETREAS clear), CPACR |= 0xF00000 (FPU on),
   **CONFIG_GPIO_AS_PINRESET**: if UICR PSELRESET[0] (0x10001200) or [1] (0x10001204) is unset (bit31=1): NVMC CONFIG=1,
   write 0x12 (P0.18) to both, CONFIG=0, `AIRCR=0x05FA0004` (system reset). SystemCoreClock = 64 000 000.
3. `__main` 0x200: SP=0x20004218, scatter-load (table 0x4c8–0x4e8: copy 8 B 0x4e8→0x20002210 = {flag=1, SystemCoreClock=64 MHz};
   zero 0x20002218..0x20004218), then `main` 0x490.
4. **main 0x490 – the only boot decision:**
   ```c
   if (*(u32*)0x27010 == 0x706d756a /*"jump"*/ && *(u32*)0x50000 != 0xFFFFFFFF) {
       *(u8*)0x20002210 = 0;  boot(0x50000);      // application
   } else {
       *(u8*)0x20002210 = 1;  boot(0x28000);      // bootloader
   }
   boot(v) (0x214): MSP = *(v); pc = *(v+4);
   ```
   No GPREGRET, no pin/button check, no CRC check in the stub (C: no reference to 0x4000051c/520 anywhere in flash).
   CRC is checked only by the bootloader when it writes the "jump" magic.

**Interrupt forwarding 0x220 (C):**
```
r0 = IPSR & 0x3F;                       // exception number
base = (*(u32*)0x20002210 & 1) ? 0x28000 : 0x50000;
bx *(u32*)(base + 4*r0)
```
VTOR is never written by any image (no 0xE000ED08 reference) → all exceptions of the app go through this ~8-cycle
trampoline, and **the app's RAM word 0x20002210 decides the vector table**.
- Bootloader `main` (0x2bcf8) writes `*(u32*)0x20002210 = *(u32*)0x28200 = 0x02010101` (bit0=1) (C).
- App RW init leaves 0x20002210 = 0 (C, ram_init.bin). But the app later stores the return value of
  `nrf_esb_write_payload` (0x5c490) into 0x20002210 (0x5ed06, 0x5ed12, 0x62b3c) (C). Return codes are 0/4/6/8/0xE
  (even) in normal use, but 7 and 9 (INVALID_PARAM/LENGTH) are possible → would route all IRQs to the bootloader's
  handlers. Latent bug of the stock FW (H on real-world impact). **Replacement firmware: either keep 0x20002210 bit0=0,
  or simply set VTOR=0x50000 at startup (or put your own vector table at 0x0 and drop the stub).**

---

## 3. Header page 0x27000 (C, layout from bootloader code 0x2a2e4 / 0x2a390 / 0x2f750, app 0x56cd8)

Current contents: `00002800 00000500 00000500 0006719c 706d756a ffffffff 00007e60 736e6148`

| Off | Current | Meaning |
|---|---|---|
| +0x00 | 0x00280000 | never read or written by any code (preserved by read-modify-write). Meaning unknown (factory value) |
| +0x04 | 0x00050000 | never read by code (preserved). Probably "app start" (H) |
| +0x08 | 0x00050000 | CRC range start (= DFU base address) |
| +0x0C | 0x0006719C | CRC range end (exclusive) = last written DFU address |
| +0x10 | "jump" 0x706d756a | boot status: "jump" = boot app; "fail" 0x6c696166 = CRC failed; 0xFFFFFFFF = stay in BL |
| +0x14 | 0xFFFFFFFF | if == **0x27981299**, finalize skips CRC ("Fred" tag). Never written by code except to 0xFFFFFFFF |
| +0x18 | 0x7E60 (u16) | CRC-16 of [+8,+0xC) |
| +0x1C | "Hans" 0x736e6148 | result tag: "Hans" = CRC ok, "Fred" = bypass magic, "_PMX" 0x584d505f = fail |

**CRC algorithm** (0x2a5c0 = Nordic `crc16_compute`, CRC-16/CCITT-FALSE, init 0xFFFF, poly 0x1021, no reflection,
no xorout). Verified: crc16(flash[0x50000:0x6719c]) = **0x7E60** = stored value (C).
```python
def crc16(data, crc=0xFFFF):
    for b in data:
        crc = ((crc >> 8) | (crc << 8)) & 0xFFFF; crc ^= b
        crc ^= (crc & 0xFF) >> 4; crc ^= (crc << 12) & 0xFFFF
        crc ^= ((crc & 0xFF) << 5) & 0xFFFF
    return crc
```
The header SP/reset pair "0x00280000 / 0x00050000" is therefore **not** a vector table – just these two words.

---

## 4. Firmware update (DFU) mechanism

### 4.1 Entering the bootloader (C)
Only way: clear "jump" at 0x27010 and reset. App function **0x56cd8**: copy 28 B of 0x27000 to RAM 0x200084f0, set +0x10 =
0xFFFFFFFF, page read-modify-write through 0x57628 (erase 0x27000 + write non-0xFFFFFFFF words), wait 300 ms, `NVIC_SystemReset`
(0x51798). Called from:
- **Vendor command** in handler 0x51c44 (0x533d0): `A1 00` with `[2..3]=0xB44B, [4..7]=0x27981094, [8..11]=0x01001024`,
  only if unlocked (`0x20002811`=1, set by `A0 EA` + `[2..3]=0xA55A`, cleared by `A0 DA`).
  Handler is reached from the USB HID vendor OUT report (0x58b46, reply via IN report 0x55cee) **and** from radio
  packets of type 2 relayed by the dongle (0x546d8 → 0x50578 → 0x50682) (C).
- **Button combo** 0x63f02: on USB event when USB state==5 and pending event code 4 (0x599f0). Event 4 is generated at
  0x61ce0 when unlocked (0x20002811) and button mask (0x200024c4) == 7 (H: three buttons held).

### 4.2 Bootloader (0x28000) overview (C)
main 0x2bcf8: set IRQ flag, WDT init (0x30f64: reload **3000 ms**, CONFIG=1 = run in sleep; fed in main loop via 0x2e074),
clocks + **REGOUT0 fix-up 0x2a4a8: if UICR REGOUT0 (0x10001304) != 1 → NVMC ERASEUICR (whole UICR!) then write REGOUT0=1**
(this is why APPROTECT/PSELRESET/NFCPINS get rewritten by the next SystemInit), timers, USB, SPI0 pins, LED pins
P0.04/06/08/P1.09/P0.11/12 set to output, `app_usbd` HID generic init (0x2e9fc), loop: usbd event queue, WFE, WDT feed.
Bootloader SystemInit (0x28640): same errata + NFCPINS→GPIO (write 0xFFFFFFFE, reset) + PSELRESET=P0.18.
**No RADIO access in the bootloader (no 0x40001xxx reference) → DFU is USB only** (C).
Peripherals used by BL: POWER/CLOCK, SPI0 (0x40003000), WDT, RTC1 (app_timer), NVMC, USBD (+0x4006ECxx errata regs).

USB identity (device descriptor 0x32590): **VID 0x0951, PID 0x16F8**, bcdDevice 0x0101, strings "Kingston",
"HyperX Pulsefire Dart". HID report descriptor 0x3132c: usage page 0xFF13, usage 1, 64-byte IN (usage 2) and
64-byte OUT (usage 3), **no report ID**. (App: PID 0x16E2, bcdDevice 0x1108, same vendor descriptor at 0x64744.)

### 4.3 DFU protocol (bootloader handler 0x2e15c, called from HID OUT event 0x2af90) (C)
Request = 64-byte OUT report; response = 64-byte IN report (zeroed, then):
`resp[0]=req[0], resp[1]=req[1], resp[2..3]=len (u16 LE, default 2), resp[4]=0xEC, resp[5]=status`.
Status: **0xAC** ok, **0xFE** rejected/locked (default), 0xFD size too big, 0xFC bad mode, 0xFA CRC mismatch.
All multi-byte fields little-endian. Everything except `A0` requires the unlock flag (0x2000242c).

| Cmd | Request | Action |
|---|---|---|
| `A0 EA` | – | unlock → AC |
| `A0 DA` | – | lock → AC |
| `A2 01` | [2..3]=0xA55A | version: len=6, resp[4]=*(0x28201)=0x01, resp[5]=*(0x28202)=0x01 (overwrites status byte) |
| `A1 00` | [2..3]=0xB44B,[4..7]=0x27981094,[8..11]=0x01001024 | ack only (AC) in BL |
| `A1 03` | [2..3]=6, [4]=mode, [5..7]=image size (24-bit) | size > **0x9D000** → FD; mode 0/2 → FC; **mode 1 = start**: header +0x10,+0x14 := 0xFFFFFFFF (erase 0x27000, rewrite 7 words), erase page 0x50000, base=cur=0x50000, erased_end=0x50FFF → AC |
| `A1 04` | [2..3]=packet index n, [4..63]=60 data bytes | addr = 0x50000 + n·60; cur_end = addr+60; if cur_end > erased_end → erase page of cur_end; write 15 words at addr → AC |
| `A1 07` | [2..3]=2, [4..5]=CRC16 | crc16(0x50000 .. cur_end); equal → header +8=0x50000, +0xC=cur_end, +0x18=crc, resp len=4, AC, resp[6..7]=crc; else FA + computed crc |
| `A1 02` | [2..3]=0x9669,[4..7]=0x33226677,[8..11]=0xCCDD9988 | **finalize** (0x2a2e4): if +0x14==0x27981299 → "jump"/"Fred"; elif crc16(+8..+0xC)==+0x18 → "jump"/"Hans"; else "fail"/"_PMX"; rewrite header page; reply AC, ~100 ms delay, `NVIC_SystemReset` |
| `A1 11`, `A2 05/06/10` | | no-op |

Notes / weaknesses (C from code): no signature, no encryption, only CRC-16; packet index is not bounds-checked
(n·60 can go past 0xED000); pages are erased lazily in order, so packets must be sent in ascending order;
cur_end is the end of the **last** packet sent, so the last packet must be the highest index.
Max image = 0x9D000 (0x50000–0xED000). A "fail" leaves the device in the bootloader (stub will not boot the app).
Dead code: USB event handler 0x30dcc also calls finalize if flag 0x200022dc is set, but nothing ever sets it.

### 4.4 NVMC routines (C)
Nordic `nrf_nvmc`-style, CONFIG at 0x4001E504 (1=WEN, 2=EEN), READY 0x4001E400 polled, ISB/DSB after CONFIG writes:

| Image | erase page | write word | write words | wait-ready |
|---|---|---|---|---|
| app | 0x5cf78 (ERASEPAGE=0x4001E508) | 0x5cfd8 | 0x5d038 (addr, src, nwords) | 0x64034 |
| bootloader | 0x2d33c | – | 0x2d39c (addr, src, nwords) | 0x30f50 |

App `nrf_fstorage` instance 0x20005fc8: range **0x27000–0x100000**, NVMC backend 0x64598 (erase 0x57740 checks page
alignment via FICR CODEPAGESIZE 0x10000010; write 0x577a0 skips 0xFFFFFFFF words). RMW helper 0x57628 (addr, src, nbytes):
malloc 4 KB page buffer, merge, erase, write. Flash read helper 0x5760e (word copy).
Bootloader `nrf_fstorage` instance 0x20003fac: range 0xED000–0xEF000, handler 0x2e591 (no writer found).

---

## 5. Persistent settings

### 5.1 Page 0xEF000 – radio pairing record (C for layout/usage, H for byte1)
Contents: `02 02 6e 2f 90 c4 01 ff` (rest 0xFF). RAM mirror **0x20002232** (8 bytes, unaligned).

| Byte | Value | Meaning |
|---|---|---|
| 0 | 0x02 | **RF channel** → `nrf_esb_set_rf_channel` (0x5c3b4) and 0x20002310. Unpaired default = 100 (0x5ae00) |
| 1 | 0x02 | always written as 2 (0x54740). Read only for the "get pairing" command (0x5332c). Record version/pipe? (H) |
| 2–5 | 6e 2f 90 c4 | **ESB base address 1** from the dongle → `nrf_esb_set_base_address_1` (0x5c380); unpaired = cc cc cc cc |
| 6 | 0x01 | 1 = paired/valid (checked 0x5acfa, 0x5ad9a, 0x5a23e, 0x5aaae) |
| 7 | 0xFF | written as 0xFF |

Radio init 0x5acbc: read page → base address 0 = `ee ee ee ee` (0x5ae28), pairing mode uses base1 `31 32 33 34`
("1234", 0x5ae2c), `update_prefix`-style calls 0x5c458(0,1) and (2,2) (H: pipe/prefix meaning – see radio report).
Write path: pairing RX event (0x546d8, pairing mode flag 0x2000223d) fills 0x20002232 from the dongle packet
([0]=channel, [2..5]=address, [1]=2,[6]=1,[7]=0xFF) and sets state 0x2000223b=3; then 0x50d10 erases 0xEF000 and
writes 2 words, and also appends the record to the EEPROM ring (below). Command `?? 13` (0x5330e/0x53416) reads the
record; command at 0x5347e (`[3]=0xEA`) sets it from host data.

### 5.2 External I²C EEPROM (addr 0x50) map (C for addresses/sizes/RAM buffers, H for semantics)
Access routine **0x50c40(addr16, buf, len, write)**: big-endian 16-bit memory address, TWI1 via nrfx_twi (instance
0x64674), after a write waits 30 ms (0x1e × 1 ms). Boot sequence in main 0x59f14:

| EEPROM addr | Len | RAM buffer | Notes |
|---|---|---|---|
| 0x0001 | 4 | – | stored FW version (compared with 0x20002278 = 0x01010008; written nibble-split) |
| 0x0010 | 4 | – | layout signature, 4 nibbles; ≠ 0x2000227c (0x4707) → write all defaults below |
| 0x0020 | 1 | 0x20002809 | |
| 0x0060 | 9 | 0x200076df | |
| 0x0090 | 0x26 | 0x20007702 | |
| 0x0110 | 0x1a | 0x200076e8 | per-profile table (stride 13, 2 entries?) used by cmd handler 0x51f3e (H: DPI) |
| 0x0190 | 0x18 | 0x20007728 | |
| 0x0210+16·i, i=0..5 | 9 | 0x20007740+0x209·i | read via 0x51258 (H: 6 macro/button slots) |
| 0x0290 | 0x1b | 0x200076c4 | |
| 0x0490 / 0x0500 | 0x70 / 0x20 | 0x20008385 | |
| 0x0690 / 0x0700 | 0x70 / 0x20 | 0x20008415 | |
| 0x1490+8·(n−1) | 8 | 0x20002232 | pairing-record ring, 63 slots |
| 0x1690 | 4 | 0x20002248 | ring index n (1..63, wraps 0x40→1) |
| 0x16a0 | 1 | – | 1 = pairing already migrated from 0xEF000 to EEPROM |
| 0xfe00 | 1 | – | dummy read at start of main |

(A second I²C device at 0x55, context 0x20002600, is read by 0x50e2c/0x57878 – looks like a fuel gauge, not storage.)

### 5.3 Other flash data pages
- 0xEE000: 0x565ac writes 2 bytes from 0x20002774 (byte1 = mode 0..2 selected by button combos 1–3 at 0x61d50).
  No code reads 0xEE000 back (C by literal search); currently erased in the dump.
- No other page, no Nordic FDS/peer-manager records.

---

## 6. UICR usage (C)
Dump: PSELRESET[0]=PSELRESET[1]=0x12 (P0.18), NFCPINS=0xFFFFFFFE (GPIO), REGOUT0=1 (2.1 V), everything else erased
(APPROTECT open, **CUSTOMER[] empty**, NRFFW/NRFHW empty).
- Stub SystemInit writes PSELRESET (0x3d2). Bootloader SystemInit writes NFCPINS + PSELRESET (0x2878e–0x28832).
  App SystemInit writes NFCPINS and REGOUT0 (if (REGOUT0&7)==7 → 1, then reset) (0x53f2a–0x53fb2).
- Bootloader erases the whole UICR if REGOUT0 != 1 (0x2a4a8).
- **No code reads UICR CUSTOMER registers** (no 0x10001080–0x100010FC reference; only UICR base+0x200/0x204/0x20c/0x304).

---

## 7. ARM scatter-load / initialized RAM (C)

armlink "RW compression" decompressor (identical code at stub-less images: app 0x50520, BL 0x2848c):
```
token b = *src++
lit = b & 7;  if (!lit) lit = *src++;      // copy lit-1 literal bytes
run = b >> 4; if (!run) run = *src++;
copy lit-1 bytes src→dst
if (b & 8) { off = *src++; copy run+2 bytes from dst-off }   // LZ back-reference (may overlap)
else       { emit run zero bytes }
until dst reaches end
```

| Image | Region$$Table | Entry | load | exec | size | handler |
|---|---|---|---|---|---|---|
| stub | 0x4c8–0x4e8 | RW | 0x004e8 | 0x20002210 | 0x8 | copy 0x470 |
| | | ZI | – | 0x20002218 | 0x2000 | zero 0x480 |
| BL | 0x326c8–0x326e8 | RW | 0x32724 (131 B) | 0x20002210 | 0x240 | decompress 0x2848c |
| | | ZI | – | 0x20002450 | 0x3fe0 | zero 0x28af2 (top 0x20006430 = SP) |
| **app** | 0x66f24–0x66f44 | RW | 0x66fbc (462 B → 0x6718a) | 0x20002210 | 0x624 | decompress 0x50520 |
| | | ZI | – | 0x20002834 | 0x7cdc | zero 0x54c52 (top 0x2000a510 = SP) |

**`re/ram_init.bin`: base address 0x20000000**, length 0xA510 (covers 0x20000000–0x2000A510; bytes below 0x20002210
are unused/zero). All three images put RW at 0x20002210 - RAM 0x20000000–0x2000220F is never used by any image.
0x66f44–0x66fbb (after the table) holds 10 small descriptors {ptr, ptr, n, size, flag} pointing at 0x20005bxx–0x20005exx
(H: app_timer / queue definitions), not scatter data.

Useful initialized values (app): 0x2000221c.. `11 22 33 44 55 66 77 88 99 aa bb cc dd` (test pattern),
0x20002270=100, 0x20002274=0x01010009, 0x20002278=0x01010008, 0x2000227c=0x4707, 0x200022ac=5000,
ESB defaults 0x200022bc `e7e7e7e7 / c2c2c2c2 / e7 c2 c3 c4 c5 c6 c7 c8` (nrf_esb default addresses),
channel list 0x200022fa `02 1a 32 4a 08 20 38 0e 26 3e 14 2c 44 4e 35 1d 05` (H: hopping table),
TWI device 0x2000247c addr 0x50, 0x20002600 addr 0x55, USB string descriptors at 0x200026e8.
**0x20004458 / 0x20004460 are in ZI** (zero at boot, filled at runtime) – ram_init gives no values there; look for
their writers in the PWM init code instead.

---

## 8. Practical notes for a replacement firmware
- Easiest: keep stub+BL, link app at 0x50000, RAM from 0x20002210 or above, either keep word 0x20002210 bit0 = 0 or
  write VTOR=0x50000 first thing. Keep header "jump" at 0x27010, or the stub will boot the bootloader.
  The stub does not check the CRC, so a SWD-flashed app just needs `*(0x27010)=="jump"` and a non-erased 0x50000.
- To stay USB-updatable via the stock BL: image ≤ 0x9D000, send via the protocol in §4.3, CRC-16/CCITT-FALSE.
- WDT: the stock BL starts WDT (3 s). A soft reset does not stop a running WDT on nRF52 (from the Nordic product
  spec, not seen in this dump) → an app entered from the BL after a soft reset must feed the WDT or configure it itself.
  A normal power-up goes stub→app directly, without the WDT.
- REGOUT0 must stay 1, or the BL will erase UICR on its next run.

## 9. Open questions
1. Header words +0x00 (0x00280000) and +0x04 (0x00050000): no code uses them. Factory tool metadata?
2. Who ever writes the CRC-bypass magic 0x27981299 at +0x14 (factory programmer?). No code does.
3. EEPROM field meanings (DPI/LED/macros) – only addresses/sizes/RAM buffers were mapped here.
4. Pairing byte 1 (=2) semantics; ESB pipe/prefix meaning of 0x5c458 calls (radio report).
5. Bootloader IRQ4 handler 0x28615 (SPIM1/TWIM1 slot) and SPI0 use in the BL (maybe puts the sensor in shutdown) – not analysed.
6. Exact gesture for event code 4 (button mask 7 plus the other conditions at 0x61cc6).
