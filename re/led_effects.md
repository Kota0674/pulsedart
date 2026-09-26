# Stock LED lighting: full spec for a 1:1 re-implementation

Source: static analysis of `dump/flash.bin` (app 0x50000–0x68000, FW 1.1.0.8). Listing: `re/tools/out/app.lst`.
Helpers written for this task: `re/tools/fn.sh ADDR…` (print one function from app.lst), `re/tools/fnof.py ADDR…` (which function contains an address), `re/tools/gen_led_tables.py` (writes `re/led_tables.h`).

Tags: **[C]** = CONFIRMED in code or data at the given address. **[H]** = HYPOTHESIS.
Hardware facts checked on the mouse (not derived from code): zone 0 = rear logo, zone 1 = scroll wheel. PWM period is 320 at 1 MHz, and brightness is proportional to the compare value.

Changes versus `input_usb_led.md` §6:
- Effect 3 is a **reactive pulse**. It is triggered by button presses and wheel detents.
- Effect 4 does nothing: nothing plays the custom arrays.
- The 0x400 / 0x419 timer periods are in unreachable code. The real step period is `speed + 16` ms.
- The pairing timer does not blink any LED.
- DPI-indicator timing is corrected.
- Low battery: mode 1 is the critical case (SOC < 5 %, 0.5 s) and mode 2 is the low case (1 s). This matches `power.md`.

---

## 0. Building blocks

### 0.1 Timers  [C]
- Every LED animation runs on RTC1 app_timers: **timer 4 = zone 0** and **timer 5 = zone 1**.
  - Both are REPEATED timers (created at 0x5f67c / 0x5f68a, mode arg 1).
  - Their handlers go through `app_sched`: timer 4 → 0x61960 → **0x6205c**, timer 5 → 0x61988 → **0x62394**.
- `timer_start(id, ticks)` = 0x5f764. It does **nothing if the timer is already running** (bitmask 0x20002478). This is why every caller stops the timer first (0x5f81c).
- `timer_running(id)` = 0x59a18. Each tick handler first checks that its own timer is still running.
- ms → ticks conversion used everywhere (0x503e8 is a 64-bit udiv): `ticks = (ms*32768 + 500) / 1000`. This is `APP_TIMER_TICKS`, available as `LED_MS_TO_TICKS` in `led_tables.h`.

### 0.2 Output path  [C]
```
set_zone(z, bri, rgb[3])                       // 0x64428
    scale = (u16)((bri << 8) / 101)            // sdiv; bri 0..100 -> 0..253
    if (z == 1 && *(u8*)0x200024fa) return;    // no writer of 0x200024fa found -> always 0 [H]
    for c in R,G,B:
        v = (u16)(rgb[c] * scale) >> 8         // max 255*253>>8 = 252
        pwm_set(led_zone_pins[z][c], v)
pwm_set(pin, v)                                // 0x5f1a0
    ch = index of pin in led_pwm_pins (0x646c4); inst = ch>>2; n = ch&3
    seq[inst][n] = 320 - v                     // seq @0x20005d18 (PWM0), 0x20005d20 (PWM1)
    nrfx_pwm_simple_playback(inst, seq[inst], 1, NRFX_PWM_FLAG_LOOP)   // 0x5df98, restarts playback on every write
```
- Duty = v/320, so the maximum is **252/320** through the effects. Only the LED test (0x51124) writes 255 directly.
- Envelope scaling: `scale_env(in, out, idx)` = **0x58348**: `out[c] = (u16)(led_breath[idx] * in[c]) >> 8`. It uses table 0x66b52.
- LED on/off:
  - `led_on()` = 0x59c1c: if 0x20002535 == 0, run the PWM init 0x5f220. Then 0x20002525 = 1.
  - `led_off()` = 0x59c00: if PWM is initialised, run 0x5f2f4 (nrfx_pwm_uninit on both instances, then every LED pin becomes a GPIO output driven **low**). Then 0x20002525 = 0.
- P0.24 (LED rail / boost enable [H]) is set to 1 by most indications: DPI, low battery, polling, LED test, and the end of low battery / polling. See `power.md`.

### 0.3 HSV → RGB with gamma: 0x59d9c  [C]
Argument is packed `h | s<<8 | v<<16`. The result is packed `R | G<<8 | B<<16`. Table `g` = `led_gamma` (0x66996).
```
if s == 0: return g[v], g[v], g[v]
region = h / 43                      // 0..5 (h=215..255 -> 5)
rem    = (h - region*43) * 6         // code: ((3*x)<<25)>>24, identical for x<=42
p = (v * (255 - s)) >> 8
q = (v * (255 - ((s * rem) >> 8))) >> 8
t = (v * (255 - ((s * (255 - rem)) >> 8))) >> 8
region 0: (g[v], g[t], g[p])     1: (g[q], g[v], g[p])     2: (g[p], g[v], g[t])
       3: (g[p], g[q], g[v])     4: (g[t], g[p], g[v])     5/other: (g[v], g[p], g[q])
```
Only the spectrum hue cycle and the reactive random colour pass through gamma. Static, breathing and cross-fade colours are **not** gamma-corrected.

---

## 1. Data structures

### 1.1 Stored zone block, 13 bytes × 2 (RAM 0x200076e8 / +0x0d; EEPROM 0x0110 len 0x1a)  [C]
| Byte | Meaning | Written by D2 from request byte | Read by |
|---|---|---|---|
| 0 | effect: 0 static, 1 spectrum, 2 breathing, 3 reactive, 4 accepted but no-op | `req[2] >> 4` (must be < 5) | 0x5eee0 |
| 1 | sub-mode 0..3 (see §2) | `req[2] & 0xF` (must be < 4) | the tick handlers read it **live** (0x200076e8[1] / [0xe]) |
| 2 | speed 0..255 (0 = fastest) | `req[0xB]` (no range check) | handlers → state.speed |
| 3..5 | **unused**. Default `20 00 00`. D2 never writes them and no reader was found. [C for D2; H that nothing else reads them] | – | – |
| 6 | brightness 0..100 | `req[0xA]` (> 100 → error 1) | handlers → state.bri |
| 7..9 | colour 1 R,G,B | `req[4..6]` | handlers |
| 10..12 | colour 2 R,G,B | `req[7..9]` | handlers, and the tick handlers read it live |

Default (RW 0x200027b1, both zones): `01 02 20 20 00 00 32 ff ff ff 00 00 00`. This is spectrum, sub 2 (behaves like sub 0), speed 32, brightness 50, colour 1 white.

**Vendor `D2 zz em ?? r1 g1 b1 r2 g2 b2 bri spd`** (0x51f24–0x5212c):  [C]
```
z = req[1] >> 4 ; *(u8*)0x20002328 = 1
if z < 2:
    if req[0xA] > 100: err 1; stop
    blk[z][6] = req[0xA]                       // stored BEFORE the mode check
    if (req[2]>>4) >= 5 or (req[2]&0xF) >= 4: err 2; stop
    blk[z][0] = req[2]>>4; blk[z][1] = req[2]&0xF; blk[z][2] = req[0xB]
    blk[z][7..12] = req[4..9]
    led_apply(req+2, z, &blk[z], enable=1)     // 0x5eee0
elif z == 2:  (both)
    if req[0xA] > 100: err 1
    for i in 0,1: same fields into blk[i] (a bad mode sets err 2 but the loop continues)
    led_apply(req+2, 2, &blk[0], 1)
else: error
```
`req[3]` is ignored. The write is RAM only; `DE 02` saves it to EEPROM. `52 00 00` reads back eff, sub, speed, bri, and both colours per zone.

### 1.2 Zone runtime state, 16 bytes (0x200060f4 + 0x10·z)  [C]
| Off | Type | Meaning |
|---|---|---|
| 0 | u16 | zone id (0/1). This is what `set_zone` uses. |
| 2 | u8 | **mode**: 0 static, 1 breathing, 2 spectrum, 3 reactive, 6 charging, 7 DPI indicator, 8 low-battery blink, 9 polling-rate indicator |
| 3 | u8 | phase/flag: breathing colour index, reactive "colour 2 pass" flag, cross-fade flag (0/1/0x10/0x11) |
| 4 | u8 | brightness 0..100 |
| 5..7 | u8[3] | current colour R,G,B. Spectrum (sub ≠ 1) uses these bytes as H,S,V. |
| 8..10 | u8[3] | cross-fade target colour (spectrum sub 1 only) |
| 11 | – | unused |
| 12 | u8 | idx (step counter) |
| 13 | u8 | last idx (N). This is `0x20002526` = **187** for envelope modes, 255 for spectrum, 0 for static/reactive-idle. |
| 14 | u16 | speed (block[2]). 500 / 1000 in low-battery mode (unused there). |

**Backup slots** (16 bytes each, zone 0 then zone 1) [C]:
- 0x20006114 / 0x20006124: charging (mode 6)
- 0x20006134 / 0x20006144: DPI indicator (mode 7)
- 0x20006154 / 0x20006164: polling indicator (mode 9)
- 0x20006174 / 0x20006184: low battery (mode 8)

Global flags:
| Address | Meaning |
|---|---|
| 0x20002520 | LED settings applied (DPI indicator allowed) |
| 0x20002525 | LEDs on |
| 0x20002535 | PWM initialised |
| 0x20002241 | low-battery level: 0 / 1 = critical / 2 = low |
| 0x20002534 | low-battery blink phase |
| 0x20002521 | polling-indicator step counter |
| 0x20002522 | polling-indicator colour [3] |
| 0x200024e6 | LEDs suspended by sleep |

---

## 2. Apply / dispatcher

```
led_apply(src, z, blk, enable)            // 0x5eee0; z = 0, 1 or 2 (both, uses blk of zone 0 for both)
    enable ? led_on() : led_off()
    switch blk[0]:
      0: static(z, blk)                   // 0x60a3c (no pre-clear)
      1: clear(z); spectrum(z, blk)       // 0x60a04 then 0x56720
      2: clear(z); breathing(z, blk)      // 0x60a04 then 0x5636c
      3: clear(z); reactive_arm(z, blk)   // 0x60a04 then 0x5ff88
      4: nothing                          // [C] no handler; previous state/timer keep running
clear(z): set_zone(z', 0, {0,0,0}) for the addressed zone(s)   // 0x60a04 (bri 0 -> black)

led_apply_all(enable)                     // 0x606ec
    0x20002520 = 0
    led_apply(_, 0, &blk[0], enable); led_apply(_, 1, &blk[1], enable)
    0x20002520 = 1
```
Callers of 0x606ec [C]:
- boot 0x5a45e
- EEPROM load 0x5a98a
- vendor `DF AA 02` / `DF AA FF` 0x5264e / 0x526e2 (after copying the defaults)
- low battery cleared on charger 0x5a88e
- charging path 0x5a62a
- wake 0x64062 / 0x6077c
- scan 0x61d56

When `enable = 0` the PWM is stopped but the handlers still run and start their timers. [C]

Each handler has a `second_arg == 0` branch, a "restore from struct" path: 0x56732, 0x5637e, 0x5ff96, 0x507ca. That branch is the only place the 0x400 / 0x419 / 0x3d7 periods are used, via 0x64378. It is **unreachable**: the only caller of 0x56720 / 0x5636c / 0x5ff88 is 0x5eee0, and it always passes 1. The same holds for 0x507b8, whose only caller 0x50fe4 passes 1. [C]

### 2.1 Common tick (0x6205c zone 0, 0x62394 zone 1)  [C]
```
on_tick(z):
    if !timer_running(4+z): return
    S = &state[z]; snap = copy(*S)            // output uses snap.zone, snap.bri, snap.col (rgb)
    sub = blk[z][1]                           // live
    if S.mode == 2:                           // spectrum
        if sub == 1:
            snap.col = xfade_step(S)          // 0x51048
            if S.flag & 1:
                if S.flag == 1: S.tgt = blk[z].c2; S.flag = 0x10
                else:           S.tgt = blk[z].c1; S.flag = 0
            goto IDX
        S.col[0] = S.idx                      // hue; S.col[1], S.col[2] = 255
        snap.col = hsv2rgb(S.col[0], S.col[1], S.col[2])
    elif S.mode == 9:  zone0: return (no output) ; zone1: polling_step(); return      // §4.4
    elif S.mode == 8 and z == 1:              // zone 1 only; zone 0 in mode 8 has no timer running
        lowbat_step()                         // sets snap.col and goes to IDX, or restores and returns (§4.2)
    else:
        snap.col = scale_env(S.col, S.idx)    // 0x58348
IDX:
    if S.idx < S.last: S.idx++
    else switch S.mode:
        6: charging_end(z)       (§4.3; some paths return without output)
        7: dpi_end(z)            (§4.1; replaces snap with the restored state)
        3: reactive_end(z)       (§3.4; may return without output)
        1: breathing_end(z)      (§3.3)
        default: S.idx = 0
    set_zone(snap.zone, snap.bri, snap.col)
    if S.mode == 0: timer_stop(4+z)
```

---

## 3. User effects

### 3.1 Effect 0: static (0x60a3c)  [C]
```
timer_stop(4+z)
S = {zone=z, mode=0, bri=blk[6], col=blk[7..9], idx=0, last=0, speed=0}
set_zone(z, blk[6], blk[7..9])
```
Sub and speed are ignored. No timer runs.

### 3.2 Effect 1: spectrum (0x56720)  [C]
```
timer_stop(4+z)
S.zone=z; S.mode=2; S.bri=blk[6]; S.speed=blk[2]; S.flag=0
if blk[z][1] == 1:  S.col = blk.c1; S.tgt = blk.c2       // cross-fade
else:               S.idx = 0; S.last = 255; S.col = {0, 255, 255}   // H,S,V
timer_start(4+z, ticks(blk[2] + 16))                    // step = speed+16 ms
```
- **Hue cycle** (sub 0, 2 and 3):
  - The hue goes 0,1,…,255 and then back to 0: one step per tick, 256 ticks per cycle.
  - S = V = 255, and the colour passes through gamma, then brightness.
  - Period = 256 × (speed+16) ms. The default speed 32 gives 48 ms/step, so **12.29 s per cycle**.
  - The first visible colour (after the black pre-clear) is hue 0, red, one step after apply.
  - Both zones have independent timers. They stay in phase only because they are applied together.
- **Cross-fade** (sub 1): `xfade_step` (0x51048) moves each channel of `S.col` 1 unit toward `S.tgt` per tick. The output is raw (no gamma).
  ```
  xfade_step(S): f = S.flag; r5 = (f & 1) ^ 1; r6 = f
      for c in R,G,B: if S.col[c] != S.tgt[c]: r6 = f; S.col[c] += (S.col[c] > S.tgt[c]) ? -1 : +1
                      else: r6 = r5
      S.flag = f | r6; return S.col
  ```
  - The end-of-leg test depends **only on the B channel** (the last value of `r6`). This is a vendor quirk, and the loop has to be reproduced exactly for a 1:1 look.
  - The net effect is a ping-pong c1 → c2 → c1 … with a 1–2 tick pause at each end.
  - The leg length is max|Δchannel| ticks, provided B does not finish first.

### 3.3 Effect 2: breathing (0x5636c)  [C]
```
timer_stop(4+z)
S = {zone=z, mode=1, col=blk.c1, bri=blk[6], idx=0, last=187, speed=blk[2], flag=0}
timer_start(4+z, ticks(blk[2] + 16))
```
- On every tick, `out = c * led_breath[idx] >> 8`, where idx runs 0..187. That is **188 steps × (speed+16) ms**: 9.02 s per breath at speed 32.
- The envelope (0x66b52) is:
  - 0 at idx 0
  - a ramp up to 255 at idx 63
  - 255 held through idx 94
  - a ramp down to 0 at idx 157
  - dark from idx 157 to 187
- Speed is inverted: 0 is fastest (16 ms/step, 3.0 s per breath) and 255 is slowest (271 ms/step, 51 s per breath).
```
breathing_end(z):        // idx reached 187 (the tick with idx 187 was output as 0)
    if sub == 1: S.flag ^= 1; S.col = blk.c1 if S.flag==0 else blk.c2         // alternate c1/c2 per breath
    elif sub == 2: S.flag = (S.flag + 1) % 9; S.col = led_cycle_colours[S.flag] // 0x66c1a
    S.idx = 0                                                                  // sub 0/3: same colour
```
- With sub 2 the first breath uses c1. After that the colours come from the table, starting at index 1: `aa5500, 55aa00, 00ff00, 00aa55, 0055aa, 0000ff, 5500aa, aa0055, ff0000, aa5500…`

### 3.4 Effect 3: reactive (0x5ff88 arm, 0x600b4 trigger)  [C]
Arming, from D2 or apply:
```
timer_stop(4+z)
S = {zone=z, mode=3, col=blk.c1, bri=blk[6], idx=0, last=0, speed=blk[2]}   // no timer: stays dark
```
- **Trigger** (0x600b4) fires on:
  - **every button press** (event byte [5]==0 dequeued in 0x58370 at 0x583cc; any of the 6 buttons, press only), and
  - **every wheel detent** (0x589d8 → 0x641ac returns 1, 0x589e2).
- Buttons blocked by a running combo (0x200024c3) do not trigger.
```
reactive_trigger():                      // for each zone whose S.mode == 3
    timer_stop(4+z); S.idx = 0; S.flag = 0; S.last = 187
    if sub == 1: S.col = blk.c1
    elif sub == 3:
        if z == 1 and state[0].mode == 3: S.col = state[0].col          // wheel copies the logo colour
        else: h = rand_hue(S.col[0]); S.col = hsv2rgb(h, 255, 255)      // 0x53688 + 0x59d9c
    // sub 0/2: S.col unchanged (= c1 from arming)
    timer_start(4+z, ticks((S.speed + 32) >> 2))                        // (speed+32)/4 ms per step
rand_hue(old): RNG START; repeat x = RNG.VALUE (0x60788) until |x - old| > 10; return x   // 0x53688
reactive_end(z):                         // idx reached 187
    timer_stop(4+z)
    if sub == 1:
        S.flag ^= 1
        if S.flag == 1: S.col = blk.c2; S.idx = 0; timer_start(4+z, ticks(S.speed + 16)); return   // 2nd pulse, slower formula
        else: return                                                   // done, no output this tick
    // sub != 1: output this tick (envelope[187] = 0 -> dark), timer already stopped
```
- One pulse is the same 188-step envelope. At speed 32 the step is (32+32)/4 = 16 ms, so a pulse lasts **3.0 s**.
- A new trigger restarts the pulse from idx 0.
- Sub 3 passes `S.col[0]`, the previous **R** byte (not the previous hue), to `rand_hue`. This is a vendor quirk.

### 3.5 Effect 4 / custom arrays  [C]
- `D0 01` / `D0 02` fill 0x20008385 / 0x20008415 and cfg[0x19+z].
- These arrays are referenced only by the command handler 0x51c44, the defaults 0x53ad4, save 0x53b64, EEPROM load 0x59f14 and USB read-back 0x58afc.
- **No LED code reads them**, and `led_apply` ignores effect 4.
- Selecting effect 4 leaves the previous effect running. The vendor's host software probably drives per-key colours itself, or the feature is unfinished. [H]

---

## 4. System indications

### 4.1 DPI indicator (0x50b10, arg = new stage)  [C]
Called from the DPI button actions in 0x58370 (0x58742, 0x58772, 0x587de, 0x5880e). It is **not** called from vendor `D3`.
```
if !0x20002520: return
timer_stop(5); timer_stop(4)
if state[0].mode not in (6,7) and !0x20002241:
    backup[0x20006134] = state[0]; backup[0x20006144] = state[1]
for z in 0,1: state[z] = {zone=z, mode=7, col=DPI_colour[stage] (0x20007719+3*stage), bri=50, idx=0, last=187}
timer_start(5, 0x3d7); timer_start(4, 0x3d7)          // 983 ticks = 30.0 ms/step
P0.24 = 1
```
- The zones show the same breath envelope, so one pulse is 188 × 30 ms = **5.64 s**. It is lit from idx 1 to 156, about 4.7 s, and at full level from 1.9 s to 2.85 s.
- A second press during the pulse restarts the pulse. The backup is not overwritten.
- `dpi_end` for zone 0 (0x6221e): `state[0] = backup0; snap = state[0]`. The tick then outputs the restored `col` bytes raw, at the restored brightness, and stops timer 4 if the restored mode is 0.
- `dpi_end` for zone 1 (0x627c4): `if !0x20002241: state[1] = backup1; snap = state[1]`. Then **0x20002241 = 0** and 0x200022a4 = 0.
- Vendor quirks after restore [C]:
  1. The restored effect keeps running on the **30 ms** timer, not its own (speed+16) period, until the next `led_apply`.
  2. For spectrum, the first restored output shows the H,S,V bytes as RGB for one tick.
  3. If low battery was active, zone 0 restores a stale backup.

### 4.2 Low battery (0x51168; decision at 0x5a866–0x5a8dc)  [C]
The decision runs in the main loop after the gauge SOC (0x2000226c) is updated:
```
if on external power (0x200024fb & 3):
    if 0x20002241: 0x20002241 = 0; P0.24 = 1; led_apply_all(1)
elif SOC <= misc[2] (0x200076df[2], default 15):
    if SOC < 5:  if 0x20002241 != 1: 0x20002241 = 1; lowbat_start(1)   // critical
    else:        if 0x20002241 != 2: 0x20002241 = 2; lowbat_start(2)   // low
else: 0x20002241 = 0                                                    // cleared -> zone-1 tick restores
```
```
lowbat_start(m):                 // 0x51168
    timer_stop(4); timer_stop(5)
    backup[0x20006184] = state[1]; backup[0x20006174] = state[0]
    static(1, {bri 50, col FF0000}); static(0, {bri 50, col 000000})   // via 0x60a3c: wheel red, logo off
    state[1].mode = 8; state[0].mode = 8; 0x20002534 = 1
    m==1: timer_start(5, 0x4000); state[1].speed(+0x1e of base) = 500
    m==2: timer_start(5, 0x8000); ... = 1000
    P0.24 = 1
lowbat_step() (zone-1 tick, mode 8):     // 0x62464
    if 0x20002241:
        0x20002534 ^= 1; snap.col = 0x20002534 ? FF0000 : 000000; -> IDX/output at bri 50
    else:                                  // level cleared on battery
        P0.24 = 1; timer_stop(5); timer_stop(4)
        state[1] = backup 0x20006184; state[0] = backup 0x20006174
        state[1].idx = 0; state[0].idx = 0
        timer_start(5, ticks(state[1].speed + 16)); timer_start(4, ticks(state[0].speed + 16))
        0x200022a4 = 0; return
```
- Result: **only the scroll wheel blinks red** and the logo is off. Critical (< 5 %) = 0.5 s on / 0.5 s off. Low (≤ threshold) = 1 s on / 1 s off. The first toggle turns red off one half-period after the start.
- Quirk: the restore path restarts both timers unconditionally. A restored **static** zone then outputs `col × env[0] = 0`, so it goes dark, and its timer stops. The plugged-in path does not have this problem because it calls `led_apply_all`.

### 4.3 Charging (0x50fe4 → 0x507b8, mode 6)  [C]
Called from the power state machine at 0x5a5a6 / 0x5a5fc (P0.14 path and the 5000-loop path; see `power.md` §3.1).
```
blk = {speed 30, bri 50, c1 FFFFFF}; led_on()
for z in 0,1: if state[z].mode != 6:
    timer_stop(4+z); backup[0x20006114 + 0x10*z] = state[z]
    state[z] = {zone=z, mode=6, col=FFFFFF, bri=50, idx=65, last=187, speed=30}
    timer_start(4+z, ticks(30))                     // 30 ms/step (no +16 here)
```
The result is **white breathing on both zones**, 188 × 30 ms = 5.64 s per cycle. It starts at idx 65, which is at full level. `charging_end(z)` runs when idx reaches 187 (state 0x200024fb, P0.09 = pin 9):
- **Battery** (`state & 3 == 0`, charger removed):
  - Restore the backup; idx = 0.
  - Stop the timer, then restart it with ticks(speed+16) if speed ≠ 0.
  - Return without output.
- **State 1 or 2:**
  - If P0.09 == 0 or state == 2: idx = 0 (keep breathing).
  - Otherwise: nothing. The output stays at env[187] = dark and the timer keeps running.
- **State 3** (P1.06 high, "charged" [H]):
  - **P0.09 == 1:** stop the timer, restore the backup, idx = 0, restart the timer with the restored speed if it is non-zero, and return.
    - Zone 1 only: if the "full" sub-state 0x20002282 == 2, set it to 3.
  - **P0.09 == 0:**
    - Zone 0: idx = 0 and return.
    - Zone 1: if full == 2, stop both LED timers, set **P0.24 = 0** (LED rail off) and set full = 3. Otherwise idx = 0.

### 4.4 Polling-rate toggle indicator (0x53714, mode 9)  [C for the code, H for the trigger]
Called at 0x5aa4c when event **12** is pending (0x200024c2 == 0x0c). No producer of event 12 was found.

The event toggles 0x20002809 between 0 and 3. That is the polling index: 0 = 125 Hz, 3 = 1000 Hz.
```
timer_stop(4); timer_stop(5); backup 0x20006164 = state[1]; 0x20006154 = state[0]
col = (new == 0) ? 00FF00 (green) : 0000FF (blue)      -> 0x20002522
static(0, {bri 50, col}); static(1, {bri 50, col, speed 0xFF})
state[0].mode = state[1].mode = 9; 0x20002521 = 0
timer_start(5, 0x2666); timer_start(4, 0x2666); P0.24 = 1    // 300 ms
polling_step() (zone-1 tick; the zone-0 tick returns immediately in mode 9):
    timer_stop(5)
    if cnt == 1 or cnt == 3: set_zone(0, 2, col); set_zone(1, 2, col)   // brightness 2 (!)
    else:                    set_zone(0, 0, col); set_zone(1, 0, col)   // off
    timer_start(5, 0x2666); cnt++
    if cnt == 5: stop 4,5; restore both from backups; idx = 0 (both); P0.24 = 1
                 timer_start(5, ticks(state[1].speed+16)); timer_start(4, ticks(state[0].speed+16))
```
Sequence:
- t=0: colour at brightness 50
- 300 ms: off
- 600 ms: brightness **2** (v ≤ 4/320, barely visible)
- 900 ms: off
- 1200 ms: brightness 2
- 1500 ms: off, then restore

Restoring a static zone has the same dark-static quirk as §4.2.

### 4.5 Pairing  [C]
- Entering pairing is event 6 (0x51b84). It calls `0x61314(1)`, which sets 0x200024d5 = 1, sets the counter 0x200024c1 = 1, and starts **timer 3** at 0x4000 (0.5 s).
- The timer-3 tick (0x62014) only does this: counter++, reset the idle counter (0x5661c), and stop at count 0x78 (60 s) or when the counter wraps to 0 (0x61808).
- **No LED is changed while pairing.** The zone effect keeps running.
- On a radio event 3 while 0x2000223d == 1 (pairing success [H]), 0x5e9dc runs:
  - It drives P0.11 and P0.04 (the two **green** channels, from `led_pair_pin_a/b[1]`) low and then high, as plain GPIO.
  - It sets counter = 0xF9 and restarts timer 3, which stops after 7 × 0.5 s = 3.5 s.
- While PWM owns those pins the GPIO writes probably have no visible effect. [H] Nothing turns them back.

### 4.6 LED test (0x51124, from factory-test loop 0x51818 at 0x51c38)  [C]
PWM init, then R and G = 0 on both zones and **B = 255 on both zones** (P1.09, P0.06), then P0.24 = 1.

### 4.7 Idle, sleep, USB suspend, wake, boot  [C]
- **Idle stage 1** (0x2000224c = 1 after 10.5 s): **no LED change**. No LED code reads 0x2000224c; 0x5aeaa tests only == 2.
- **Deep sleep** (0x56d28) and **USB suspend** (0x56e2c) both call 0x64074. The first time only (flag 0x200024e6) it does:
  - `led_off()`: PWM uninit and all six LED pins driven low.
  - 0x59c38: stop timers 4/5 and memset **state[0]** 16 bytes. It does this twice; the loop never advances the pointer, so state[1] is not cleared.
  - 0x20002520 = 0.
  - P0.24 is left high.
- **Wake** (PORT wake handler 0x5fb58, or 0x60754): `0x64044(1)`. If 0x200024e6 is set: P0.24 = output 1, `led_apply_all(1)` (effects restart from their initial state), then clear 0x200024e6.
- 0x60754 also calls `led_apply_all(1)` when the activity state ≠ 4 and the LEDs are off (0x20002525 == 0).
- **Boot** (0x5a3ec): powered with P0.09 == 0 → P0.24 = 0 and no apply. Otherwise → `led_apply_all(1)`. The charging effect follows later from the power loop.

### 4.8 Priority / interaction summary
| Active | DPI press | Low battery starts | Charging starts | Settings change (D2 / apply) |
|---|---|---|---|---|
| user effect | backup + DPI pulse | backup + blink | backup + white breath | replaces the state (no backup) |
| DPI (7) | restarts the pulse | overrides; the DPI state goes into the low-battery backup and resumes after the restore | backs up the DPI state (quirk) | replaces |
| low battery (8) | DPI pulse; zone 0 restores a stale backup | – | overrides | replaces |
| charging (6) | DPI pulse, no backup | overrides | skipped (mode 6) | replaces |

---

## 5. Tables (`re/led_tables.h`)
| Array | Source | Size | Use |
|---|---|---|---|
| `led_gamma` | 0x66996 | 256 | HSV → RGB only |
| `led_breath` | 0x66b52 | 188 | 0x58348 envelope; idx 0..187 (`0x20002526` = 187) |
| `led_breath_inv_unused` | 0x66a96 | 188 | Complementary envelope, no literal reference [H: dead] |
| `led_cycle_colours` | 0x66c1a | 9×3 | Breathing sub 2 |
| `led_zone_pins` | 0x66c14 | 2×3 | Zone/channel → pin for 0x64428 |
| `led_pins_alt_unused` | 0x66c0e | 6 | No reference [H] |
| `led_pwm_pins` | 0x646c4 | 8 | Pin → PWM instance/channel |
| `led_pair_pin_a/b` | 0x66948 / 0x6694a | 2 + 2 | Pairing-success GPIO |

- The range "0x66a93..0x66bf0" in the task covers the end of gamma (0x66a93–0x66a95), the unreferenced complementary envelope, and `led_breath`.
- The bytes `00 40 80 c0 ff` at 0x66c35 have no literal reference and were not exported.

---

## 6. Open questions
1. **Event 12** (polling-rate toggle, §4.4): no producer was found by literal search. It may be set through a computed address, or be dead. [H]
2. **0x200024fa** (zone-1 output inhibit in 0x64428): no writer was found, so it is treated as always 0. [H]
3. **Pairing-success GPIO writes** (0x5e9dc): are they visible while the PWM owns the pins? Hardware test.
4. Should the new firmware reproduce the vendor quirks, or fix them?
   - The 30 ms timer after a DPI restore.
   - The one-tick HSV-as-RGB glitch.
   - A static zone going dark after a low-battery or polling restore.
   - The B-only end test in the cross-fade.
   - `rand_hue` seeded with the R byte.
   - Brightness 2 in the polling indicator.

   Recommendation: reproduce the timings and colours, and fix the glitches.
5. Block bytes [3..5] (`20 00 00` default) have no known reader. The host software may use them. [H]
6. Charging state 3 and the P0.09 semantics depend on the unresolved power pins (see `power.md` §3).
