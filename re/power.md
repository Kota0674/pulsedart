# Power, battery, charging and sleep - stock firmware (main app 0x50000–0x68000)

Static analysis of `dump/flash.bin`. Live data used only from `dump/regs.txt`, `awake_regs.txt` and `watch.log`.
Tags: **[C]** = CONFIRMED (seen in code or data, with an address). **[H]** = HYPOTHESIS.
Helper scripts are in `re/tools/`: `ann.py` (annotated disassembly that names GPIO helpers and pin arguments), `gpiofn.py`, `pinuse.py` (all constant-pin GPIO calls), `rwinit.py` (decompresses the Keil RW data 0x66fbc → 0x20002210, len 0x624) and `da.py`.

---

## 0. Summary for a firmware author

* **No SAADC battery measurement.** The SAADC driver has only the IRQ handler (0x537e0, pulled in by the vector table). There is no init, channel config or sample call. Battery % and mV come from a **TI bq27421-G1A fuel gauge at I²C 0x55**. [C]
* **Two TWI1 devices** (P0.00 SCL / P0.01 SDA, 400 kHz):
  * **0x50**: 64 KB-class I²C EEPROM (16-bit addresses, 128-byte pages). It holds all settings.
  * **0x55**: bq27421 fuel gauge.
  * Neither transaction looks like a Qi receiver. The firmware does **not** talk to the Qi RX over I²C. [C]
* **Power-source state** (0x536c8, stored at 0x200024fb). [C]
  * `P1.11=1` → 0 = on battery.
  * Otherwise `P1.06=1` → 3.
  * Otherwise `P0.14=1` → 2.
  * Otherwise → 1.
  * P1.11 is active-low "external power present". P1.06 is active-low "charging" (high = not charging / charge done). [H, strong]
* **Clocks and regulators.** LFCLK = internal RC (LFCLKSRC=0), HFXO is started at boot, POFCON = 1.7 V with POFWARN → system reset. REG1 DC/DC is on. REG0 DC/DC is on only after the first sleep/wake cycle (see §4). [C]
* **"Sleep" is never System OFF.** It is a System-ON `WFE` loop with GPIO SENSE (PORT event) wake. The WDT (32.768 s, CONFIG=1 = keeps running in sleep) is **not fed in the sleep loop**. [C] So a WDT reset about every 33 s during sleep is likely. [H, needs a live check]
* **Idle → sleep on battery.** No motion for 9999 TIMER1 ticks (~10 s) → idle state (TIMER1 stopped) → 100 RTC-ms later → deep sleep. A second path uses 10.5 s + 60 s (RTC-ms). [C]

---

## 1. SAADC (0x40007000) - not used for the battery [C]

| Item | Finding |
|---|---|
| Literal refs to 0x40007xxx | Only 0x5d320 (=0x4000762C RESULT.PTR), 0x5d334/0x5d34c/0x5d358 (=0x40007000). Whole 1 MB scanned (`0x40007500` etc. never appear). No MOVW/MOVT forming 0x4000_7xxx. |
| Helpers | 0x5d314 `saadc_buffer_set(ptr,maxcnt)`, 0x5d324 `event_check(off)`, 0x5d338 `event_clear(off)`, 0x5d350 `task_trigger(off)`, 0x5d35c. They are called only from 0x537e0. |
| 0x537e0 | `SAADC_IRQHandler` (vector 7 → 0x537e1) of nrfx_saadc. Its control block is at 0x200044c4. That address is referenced only from the IRQ handler (literal 0x539a8). |
| Channel / PSELP / GAIN / REFSEL / RESOLUTION / OVERSAMPLE / TACQ | **Never written.** Nothing configures them. P0.31 (AIN7) is used as a digital pin (§3). |
| Raw → mV / % tables | None. The gauge returns mV (reg 0x04) and % (reg 0x1C) directly. The firmware does no curve or table conversion. |

For the new firmware: VDDH/5 via SAADC is possible on this chip (REG0 is used, the chip is powered from VDDH), but the stock design relies on the gauge.

---

## 2. I²C (TWI1 via nrf_twi_mngr) [C]

* Instance / config at 0x64674 / 0x6468c: SCL=0, SDA=1, FREQUENCY=0x06680000 (400 kHz), IRQ prio 4, clear_bus_init=0, hold_bus_uninit=0.
* Queue manager functions:
  * perform: 0x5d53c
  * schedule: 0x5d59c
  * transfer start: 0x6155c
  * nrfx_twi xfer: 0x5e574
  * init: 0x5e3d4
* Wrapper layer. The descriptor is `{u8 addr; u8* tx; u8 txlen; u8* rx; u8 rxlen}` (20 bytes).

  | Function | Address | What it does |
  |---|---|---|
  | `i2c_init` | 0x5f044 | Ref-counted (counter 0x200023f0) |
  | `i2c_uninit` | 0x5f11c | Called before sleep |
  | `i2c_write_read(desc)` | 0x5f09c | Write txlen bytes with NO_STOP, then a separate read of rxlen bytes |
  | `i2c_write(desc)` | 0x5f150 | Async, callback 0x59735 |

### 2.1 Device 0x50 - settings EEPROM [C]
* Descriptor 0x200060dc (copied from RW 0x2000247c: `50 …`). Access routine: `0x50c40(addr16, buf, len, is_write)`.
  * Frame: `[addrH, addrL, data…]`.
  * After a write it waits 30 ms (30 × `delay_us(1000)`, or scheduler spin if 0x20002283 is set).
  * A read is write(2) followed by read(len).
* The highest address used is 0xFE01 and page writes are ≤128 bytes → a 24C512-class part. [H]
* The map is in `boot_layout.md` §5.2. Power-related cells:

| EEPROM | Len | RAM | Meaning |
|---|---|---|---|
| 0x0060 | 9 | 0x200076df | Battery block: [0] SOC %, [1] power state, [2] low-battery threshold % (default 15), [6..7] mV, [8] gauge status (§2.2). Written at 0x5a918 and 0x53c22. |
| 0x0290 | 27 | 0x200076c4 | Config. +0x0f/+0x10 are button debounce counts. **+0x11 u32 = idle T1, +0x15 u32 = idle T2** (§5). |
| 0xFE00 | 1 | 0x2000228c | "Force gauge re-program on next boot" flag. Set by host `D1 01` (0x5ab32, followed by a reset). Cleared after the gauge is programmed (0x5a3c6). |
| 0xFE01 | 1(4) | 0x20002288 | Gauge INITCOMP wait counter (0..0x40). 0x40 = skip waiting. Cleared by host `D1 02` (0x5ab5e). |

### 2.2 Device 0x55 - TI bq27421 fuel gauge [C for register traffic; H for part number]

* Descriptor 0x20006614 (copied from RW 0x20002600 = `55 00 00 00 94 65 00 20 02 …` at 0x50d94).
* Access primitives:

| Function | Address | Behaviour |
|---|---|---|
| `write8(reg,val)` | 0x57fa4 | 2-byte write, **then 30 ms delay** |
| `read16(reg)` | 0x57824 | Write reg, read 2 bytes, little-endian |
| `control(sub)` | 0x57878 | reg0 ← sub&0xFF, reg1 ← sub>>8, then read16(0x00) |
| `seal/unseal` | 0x50ec4 | `1` → Control(0x8000) twice (UNSEAL, default keys). `0` → Control(0x0020) (SEALED). |
| block read | 0x57ebc | DataClass/Block via 0x3E/0x3F, read from 0x40+offset |

Identification:
* The code checks CHEM_ID (Control 0x0008) == **0x0128** (0x50e44). That is the bq27421-G1A chemistry (4.2 V LiCoO₂).
* The rest matches the bq27421 map: Flags 0x06 with bit4 = CFGUPMODE, OpConfig 0x3A, DataClass 0x3E, DataBlock 0x3F, BlockData 0x40.., checksum 0x60, BlockDataControl 0x61.

**Runtime reads**

| Addr | What | Where it goes |
|---|---|---|
| 0x536b4 | `read16(0x1C)` StateOfCharge % | 0x2000226c and 0x200076df[0] |
| 0x53700 | `read16(0x04)` Voltage mV | 0x20002264 and 0x200076df[6..7] |
| 0x57e5c | `read16(0x3A)` OpConfig | 0x20002290 (boot only) |
| 0x57f20 | `read16(0x06)` Flags | 0x20002298 (boot only) |
| 0x578c0 | Control(0x0000) CONTROL_STATUS | 0x20002294. Boot waits for bit 7 (INITCOMP), 100 ms per try, up to 64 tries. The count is persisted to EEPROM 0xFE01 (0x5a2e4–0x5a398). |

Polling (main loop, 0x5a804):
* Poll when the countdown 0x200022a4 == 0 and gauge status != 1.
* After a poll the countdown is set to 5000 if SOC ≠ 0, else 0 (poll every loop).
* RTC timer 9 decrements it every ~1 ms (0x62b60). So SOC and mV refresh every **5 s**.
* If gauge status == 1 (bad CHEM_ID), mV is reported as 0xFFFF and SOC as 0xFF (0x5a8e0).

**Boot configuration `0x50e2c` (called from main at 0x5a3ae if `i2c_init` returned 0)**
1. If EEPROM flag 0xFE00 ≠ 1: check CHEM_ID == 0x0128.
   * If it does not match: status (76df[8]) = 1 and stop.
   * Otherwise unseal, read State(82) bytes 35–36 (big-endian), seal. If the value == 0xFF5D the gauge is already configured: status 0, stop.
2. Otherwise status = 2 and run `0x578d0`:
   * UNSEAL (0x8000 ×2), then SET_CFGUPDATE (0x0013). Poll Flags.CFGUPMODE: 5 tries, 500 ms (0x577e0).
   * BlockDataControl(0x61) = 0.
   * Write the blocks below. Each value is written **big-endian**, with the checksum recomputed as `255 - sum` and written to 0x60.

   | Subclass | Offset | Value (hex) | Value (dec) | Meaning (bq27421 TRM) |
   |---|---|---|---|---|
   | 82 State | 0 | 0x46CD | 18125 | Qmax Cell 0 |
   | 82 | 10 | 0x0320 | 800 | **Design Capacity = 800 mAh** |
   | 82 | 12 | 0x1360 | 4960 | Design Energy (= TI default, not scaled to 800 mAh!) |
   | 82 | 16 | 0x0BB8 | 3000 | **Terminate Voltage = 3000 mV** |
   | 82 | 35 | 0xFF5D | −163 | "configured" marker / TRM field [H] |
   | 82 | 37 | 0xFEF4 | −268 | [H] |
   | 82 | 39 | 0x0002 | 2 | [H] |
   | 89 (R_a RAM) | 0..28 | 0x004C 0x004C 0x0052 0x005F 0x004D 0x004B 0x0060 0x007C 0x0089 0x008B 0x00BC 0x00E2 0x01AE 0x045A 0x06EB | | 15-point Ra table (from RW 0x20002654) |
   | 105 (0x69) | 0 | 0xFFFF | | From RW 0x20002674. Meaning unclear. [H] |
   | 64 Registers | 0 | **OpConfig = 0x05F8** | | Default 0x25F8 → BIE cleared, so the host signals battery insertion. BATLOWEN stays set. |

   Value sources: State offsets from RW 0x20002638 and values from 0x2000261c (both int arrays).
3. SOFT_RESET (0x0042) and poll CFGUPMODE clear (0x57e04), then SEALED (0x0020).
4. Control 0x000D (BAT_REMOVE) and 0x000C (BAT_INSERT) (0x50ea4/0x50eac).

Host passthrough: `51 01 00 .. r` → read16(r); `51 01 01 .. s` → Control(s). The result is a u32 LE at resp[5..8] (0x52edc). [C]

---

## 3. Power / charger GPIOs [C for code behaviour; H for hardware function]

Pin configuration:

| Pin | Config | Address |
|---|---|---|
| P1.11 | input, no pull | 0x5097a |
| P1.06 | input, no pull; in sleep: pull-up, SENSE=LOW | 0x50982; 0x56d92 |
| P0.10 | output, driven 0 at init | 0x50996 / 0x509b0 |
| P0.09 | input, **pull-down** | 0x5edf4 |
| P0.14 | input, no pull | 0x5ee04 |
| P0.24 | output, set 1 | 0x5ee0a/0x5ee10 |
| P0.25 | input pull-down, boot only | 0x51bd2 |
| P0.31 | input pull-up, pulsed at boot | 0x50da4 |

Live, on battery with no VBUS (USBREGSTATUS=0): P1.11=1, P1.06=1, P0.14=0, **P0.09=1** (high despite the pull-down, so something drives it), P0.31=1. OUT: P0.24=1, P0.10=0 (`awake.txt` OUT 0x01402000).

| Pin | What the firmware does | Likely hardware function |
|---|---|---|
| **P1.11** | 1 → state 0 "battery". All charging logic runs only when it is 0. Sleep and the low-battery warning happen only in state 0. (0x536c8, 0x5a3ee, 0x5a580, 0x5a676) | Charger **/PG** (power-good, open-drain active-low, external pull-up) [H, strong] |
| **P1.06** | With P1.11=0: 1 → state 3. Timer 9 counts how long it stays 1 (0x5094c → counter 0x20002280). After ≥1000 ms and V ≥ 4170 mV (0x104A) this is treated as "full". It is a wake source in deep sleep (pull-up, SENSE=LOW), so plugging in a charger wakes the mouse. | Charger **/CHG** or STAT (low = charging, high-Z = done / no charge) [H, strong] |
| **P0.14** | With power: 1 → state 2. If powered and P0.14=0, the no-motion timer is reloaded forever, so the mouse never idles (0x5144c). If P0.14=1, idle is allowed. It also enables the "charging" LED effect (0x5a58a → P0.24=1, 0x50fe4(0), flag 0x20002242). | "Wireless (Qi) power present" (Qi RX PG/status) vs wired [H] |
| **P0.09** | Many uses:<br>• When 0, radio payloads are held for 500 loops (0x5ece2).<br>• In path P0.14=0 with power: if 0, a 5000-loop counter then turns on the charging LED (0x5a5b4).<br>• 1 while flag 0x20002242 is set → **NVIC_SystemReset** (0x5a63e–0x5a656).<br>• 0 with flag 0x2000229c → reset (0x61ce8).<br>• In state 3: 1 → "full" LED effect, 0 → LED rail off (0x62156/0x6268a). | Active-low detect of the USB-C cable (VBUS/CC via inverter) **or** a Qi-RX status. The polarity is odd: it is high on battery. [H, weak - needs measurement] |
| **P0.10** | Set 1:<br>• in path "powered & P0.14=0" (0x5a666)<br>• when full: state 3 & ≥1000 ms & V≥4170 (0x5a742), or in the Qi-full loop (0x5a6f8)<br>Set 0:<br>• when SOC < 100 and state ≠ 1 (0x5a84c)<br>• on battery when MOTION (P0.15) is low (0x5a7e6) | Qi receiver **EN/disable** (1 = stop power transfer / EPT) [H, preferred]. A charger CE/disable is less likely, because it is also set while state 1 is powered. |
| **P0.24** | Output 1 at every LED-indication start (0x51124, 0x51168, 0x5ee10, 0x6249a…). Cleared when powered at boot with P0.09=0 (0x5a40e/0x5a434), in the Qi-full sequence (0x62726), and on powered path 0x5a638. **Not** cleared before deep sleep. | LED supply / boost enable [H] (matches `input_usb_led.md`) |
| **P0.25** | Boot (0x51bcc): pull-down, wait 10 ms, read. If 1, wait 100 ms and read again. If still 1: P0.12 (LED) on, all LED PWM set to off level, P0.24=1, and enter the **factory-test command loop** 0x51818 (codes 0xC1–0xD2). | Production test-jig strap [H, strong] |
| **P0.31** | 0x50d94 at boot (right after the gauge descriptor is set up): pull-up input. If it reads **0**, drive it low for 300 ms, then high, then back to input pull-up. | bq27421 **GPOUT**. A rising edge on GPOUT wakes the gauge from SHUTDOWN mode. With OpConfig BATLOWEN=1 it is the BAT_LOW output at other times. [H] |

### 3.1 Charging state machine (main loop 0x5a560–0x5a8ec)

Variables:

| Address | Meaning |
|---|---|
| 0x200024fb | State |
| 0x200024fc | Previous state |
| 0x20002264 | mV |
| 0x2000226c | SOC |
| 0x20002242 | Charge-LED-on flag |
| 0x20002258 / 0x2000225a | 5000-loop timer / done flag |
| 0x20002280 | P1.06-high ms counter |
| 0x20002282 | Full sub-state 0..3 |

```
state = P1.11 ? 0 : P1.06 ? 3 : P0.14 ? 2 : 1              (0x536c8, every loop)
if state in 1..3:                                           (external power)
    idle_to_sleep = 10000 (0x2000280c); lowbat_led = 0
    if P1.11==0:
        if P0.14: if !chgled: P0.24=1; led_effect 0x50fe4(0); chgled=1
        else:
            if !P0.09 && !t_done: if ++t >= 5000: t=0; t_done=1; if !0x200026c8 && !chgled: P0.24=1; led; chgled=1
                                  elif 0x200026c8: P0.24=1; chgled=0; t=0; t_done=1; 0x606ec()
                                  else: P0.24=0
            if P0.09 && chgled: chgled=0; t_done=0; NVIC_SystemReset()      # 0x5a656
            P0.10 = 1
    if P0.14 && !P1.11:
        if !P0.09:  (0x2000225a…) if prev!=3 && state==3: cnt=0; full=1
                    if cnt>=1000 && mV>=4170: full: 1→2 ; then LOOP FOREVER at 0x5a6ce:
                         {update state; scheduler 0x54e14; WDT feed 0x5e5f0; if full==3: cnt=0; P0.10=1}
                         (the LED timer handler 0x62712 does: if full==2 → stop LED timers, P0.24=0, full=3)
        else:       if state==3 && !full: cnt=0; full=1
                    if cnt>=1000 && mV>=4170: cnt=0; full=0; P0.10=1
else (battery):
    full=0; chgled=0; if P0.15(MOTION)==0: P0.10=0
    if prev&3: 0x5661c()   (reset idle counter)
after each gauge poll: if SOC<100 && state!=1: P0.10=0
    battery & SOC <= lowbat_thr(76df[2]): SOC<5 → LED mode 1 (timer5 0x4000 = 0.5 s blink) else mode 2 (0x8000 = 1 s)   (0x5a894–0x5a8d2)
    powered & lowbat_led was on → clear, P0.24=1, 0x606ec(1)
```
Note: `input_usb_led.md` reverses modes 1 and 2. The code at 0x5a8a4 uses `SOC < 5 → 0x51168(1)`.

At boot (0x5a3ec):
* If powered and P0.09=0 (either P0.14 value), P0.24 is cleared (LED rail off).
* Otherwise, timer 2 (0.5 s) is started if 0x20002469 is set, the LED start-up effect runs, and 0x606ec(1) is called.

---

## 4. POWER / CLOCK peripheral use [C]

| Register | Value / use | Address |
|---|---|---|
| DCDCEN (0x578) | 1. nrfx_power_init config `{dcdcen=1, dcdcenhv=0}` @0x644c4 at boot (0x566f4 → 0x5b2ec → 0x5dc28) | 0x5dc46 |
| DCDCEN0 (0x580, REG0) | Boot: 0 (from the same config). `0x50a64(x)`: DCDCEN0=x, then power re-init with 0x644d8[x]. x=0 → {1,0}, x=1 → {1,1}. **Before deep sleep x=0, after wake x=1**, so REG0 DC/DC is only on after the first wake. | 0x5d0ac, 0x50a64, 0x56de4, 0x56e06 |
| POFCON (0x510) | Config @0x64494: handler 0x5ee79, thr=4, thrvddh=0 → POFCON = 0x00000009 (POF=1, THRESHOLD=V17 = 1.7 V, VDDH thr=V27). Handler: `NVIC_SystemReset()` (0x5ee78 → 0x516d8). | 0x5ee60, 0x5dd1c |
| INTENSET 0x380 | USBDETECTED/USBREMOVED/USBPWRRDY for app_usbd (handler ptr 0x20002360). USBREGSTATUS.VBUSDETECT is read at 0x55406. | 0x5dde8, 0x5ddf4 |
| RESETREAS (0x400) | Only SystemInit erratum 136 (clear RESETPIN if set). No application logic. | 0x53eec |
| GPREGRET/GPREGRET2 | **Never used** (no 0x4000051C/0x520 anywhere). DFU entry instead writes 0xFFFFFFFF into the 0x27000 header +0x10, waits 300 ms and resets (0x56cd8). | – |
| SYSTEMOFF (0x500) | **Never written.** The POWER task helper 0x5b118 is only called by the clock driver. | – |
| **0x40000638** (undocumented) | 1 at boot (0x59f2e). 0 before battery sleep (0x56de0), 1 after it (0x56e0c). 1 before USB-suspend sleep (0x61f88), 0 after it (0x61f92). Purpose unknown. | – |
| LFCLKSRC (0x518) | 0 = **RC oscillator** (P0.00/P0.01 are used for I²C) | 0x5d914 |
| HFCLK | HFCLKSTART + wait at boot (0x50fc0). HFCLKSTOP before deep sleep (0x50fdc). | |
| UICR | REGOUT0 forced to 1 (2.1 V) if erased; NFCPINS → GPIO (SystemInit 0x53f2a–0x53fb2) | |
| WDT | Config @0x6449c: CRV = 32768 ms (1 073 741 ticks), IRQ prio 7, **CONFIG=1** (runs in sleep, pauses on debug halt) (0x640ea). Feed RR[ch] = 0x6E524635 from the main loop (0x5fb98/0x5ac3e) and the DFU wait (0x56d0e). WDT INTEN is cleared/set around EEPROM bursts (0x5fb88/0x5fba8). | 0x64094 |

---

## 5. Timers, idle and sleep [C unless marked]

### 5.1 Time bases
* **TIMER1** (0x541b8): 16-bit, PRESCALER=4 (1 MHz), CC[0]=1000, SHORTS COMPARE0_CLEAR, IRQ prio 7 → **1 ms tick**. It is stopped when idle is entered (0x54270, via 0x5ae58) and restarted by activity (0x54220). The ISR sets 0x2000223e, and 0x542c0 decrements counters: 0x20002314 (no-motion), 0x20002254, 0x20002230, 0x20002504, 0x20002318.
* **RTC1 app_timer**: PRESCALER=0 (32768 Hz, 0x54f62). Timer ids 1..9 have instances at 0x648b8+4·(id-1), are repeated, and dispatch through the scheduler.

  | Timer | Handler | Interval | Purpose |
  |---|---|---|---|
  | 1 | 0x61ab1 | 0x21 ticks = **1.007 ms** | Button scan + idle counters |
  | 2 | 0x61f65 | 0x4000 = 0.5 s | USB suspend / removal |
  | 5 | – | 0x4000 / 0x8000 | Low-battery LED blink |
  | 6 | – | 0x20000 = 4 s | – |
  | 9 | 0x62b61 | 0x21 | Gauge poll countdown, P1.06 counter |

### 5.2 Activity state 0x200022f9 (0x5ae6c, run each main loop)
* 0 → 1.
* 1 → `0x5acbc`, then 2 (active).
* 3 = idle: entered by 0x5ae58, which stops TIMER1. From 3, request 0x2000224c==2 → 4.
* 4 = sleep: main calls `0x56d28`. If TIMER1 is running again (0x200022a0) → back to 2.

### 5.3 Timeouts
* **No-motion (TIMER1 ticks)**: 0x20002314 is reloaded with **9999** (0x644a8) on motion (P0.15 low) or button activity.
  * On battery, expiry → idle (22f9=3), idle→sleep delay 0x2000280c = **100** (0x514be–0x514ca).
  * When powered, expiry leads to idle only if P0.14=1; the idle→sleep delay is then 10000 (5a570/61f16).
* **Idle counter 0x200024e0** (RTC ~1 ms, timer 1 handler 0x61de6–0x61f2e):
  * In state 3: after 0x2000280c ticks, if on battery → sleep request (224c=2) and reset 280c to 10000.
  * Otherwise:
    * After **cfg+0x11 = 10500** → stage 224c=1.
    * After **cfg+0x11 + cfg+0x15 = 10500 + 60000** → sleep request if on battery. If 0x2000280a is set (host `?? 54 87 .. 1`, only when 0x20002469=0) or state==2, the counter is reset instead.
  * RW defaults are 10000/60000, but boot forces 10500/60000 (0x5a280–0x5a28c).
* Result on battery: about **10 s** without motion → deep sleep. The ~5 s SWD drop seen live does not match any constant found (see §7).

### 5.4 Deep sleep on battery: `0x56d28` (System ON, WFE)
1. LED off (0x64074 → 0x611a0), GPIOTE port IRQ off (0x5ef84), stop app timers 7, 1, 9.
2. P0.15 set to pull-up. Wait while MOTION is low, reading sensor reg 0x02 to clear it (0x56630).
3. Radio off (0x6181c), `i2c_uninit`.
4. Wake sources (GPIO SENSE, PORT event handler 0x5fad5, INTENSET PORT at 0x5efd4):
   * 6 buttons P1.15/P1.13/P1.10/P1.04/P1.02/P1.00: pull-up, SENSE=LOW (table 0x66930).
   * **P0.15 MOTION**: pull-up, SENSE=LOW.
   * **P1.06**: pull-up, SENSE=LOW.
   * Wheel P0.02/P0.29 (table 0x66958, pulls 0x6695e = up): SENSE = opposite of the current level.
5. Set sleep flag 0x200023ed=1, enable GPIOTE PORT, HFCLKSTOP, POWER[0x638]=0, DCDCEN0=0 (0x50a64(0)), delay 500 ms, stop RTC1 (0x60814).
6. `nrf_pwr_mgmt` shutdown handlers (section 0x20005b00), then a **WFE loop while 0x200023ed≠0** (0x5f3ee). The PORT handler clears the flag and restarts timers 1/9 (0x5fad4).
7. Wake: RTC1 start (0x607d8), DCDCEN0=1, POWER[0x638]=1, reset the idle counter.

Things that stay powered during sleep:
* LFCLK (RC) and the WDT.
* The PMW3389, still powered because P1.07 is untouched. It must keep MOTION working.
* P0.24 stays high.
* Nothing puts the gauge into SHUTDOWN.

**WDT is not fed in this loop** → reset about 32.8 s after the last feed. [H]

### 5.5 USB-suspend sleep: `0x56e2c` (from timer 2 handler 0x61f64)
Timer-2 handler logic:
* Flags 0x20002469 = 1 and 0x20002475 = 1 → POWER[0x638]=1 → 0x56e2c.
* 0x20002469 = 1, 0x20002475 = 0 → start timer 6.
* 0x20002469 = 0 → NVIC_SystemReset (after USB removal).

0x56e2c uses the same pin setup, except that P1.06 is not a wake source and there is no DCDC/HFCLK change. The loop at 0x5fa6a processes app_usbd events. It stays in WFE while P0.15 is high and the USB bus is suspended (0x5bf7c).

---

## 6. Battery reporting to the host [C]

* **Command `51 00 00`** (dispatch 0x527b4 → 0x52e72). Reply (64-byte vendor IN report, see `input_usb_led.md`):

  | Byte | Content |
  |---|---|
  | [3] | 3 |
  | [4] | SOC % (0..100; 0xFF = no gauge) |
  | [5] | Power state 0..3 |
  | [6] | Low-battery threshold % |
  | [7..8] | Battery mV, u16 little-endian (0xFFFF = no gauge) |
  | [9] | Gauge status: 0 = OK (already configured), 1 = gauge missing / CHEM_ID≠0x0128, 2 = re-programmed this boot |
  | [10..13] | u32 LE, gauge-init wait counter (0x20002288) |

* **`D1 00 .. .. v`**: set the low-battery threshold, allowed range 5..25 (0x51ec6). `D1 94 78 .. v` sets it without the range check.
* **Async status to host**: 8 bytes from 0x20002784, sent in a 64-byte report by 0x5af0c when the countdown [8] reaches 0 (0x63e40).
  * Default content `FF 03 00 00 …`.
  * [3] = power state. It is updated and [8]=2 is set whenever the state changes (0x5a54c–0x5a55e).
  * [4] = idle flag (0x5ae90).
  * [2] = 0x20007702[0].
* No battery field was found in RF payloads (`radio.md`).

---

## 7. Open questions (need a live measurement)
1. **P0.09 polarity and source.** Log P0.09/P0.14/P1.06/P1.11 in four cases: USB cable only; Qi pad only; both; charge complete. This confirms states 1/2/3 and whether P0.09 is USB-cable detect.
2. **P0.10.** Scope it and the Qi TX pad behaviour when full on the pad. Does the pad stop (Qi EN) or does charging stop (charger CE)?
3. **P0.24.** Does it switch the LED supply only, or the sensor / other rails? Measure the current with it low.
4. **P0.31 = gauge GPOUT?** Continuity to the bq27421 pin. Also check whether the gauge is ever in SHUTDOWN at boot.
5. **WDT during sleep.** After more than 35 s asleep, read RESETREAS (expect DOG=bit1 if the hypothesis is right). Also look for periodic current spikes every ~33 s.
6. **SWD loss after ~5 s idle.** No 5 s constant leads to sleep. Candidates:
   * The TIMER1/HFCLK stop at idle entry.
   * The 5 s RTC countdown 0x200022ac (periodic sensor register write 0x539ac).
   * The undocumented POWER 0x40000638.

   Check at what point DAP access fails, and read 0x40000638 with DCDCEN/DCDCEN0 while awake.
7. **Register 0x40000638.** What it controls (VBUS/USB-related? regulator?). Read it in each state.
8. **Real sleep current and wake latency** in the WFE "deep sleep" (the RC LFCLK, WDT and sensor stay on).
9. **bq27421 State offsets 35/37/39 and subclass 105 offset 0 (0xFFFF).** Confirm the field names against the TRM. Read the gauge data memory to check Design Energy 4960 vs Design Capacity 800 mAh.
