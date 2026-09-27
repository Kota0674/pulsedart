# Async status block 0x20002784 ("FF 03") - stock mouse FW spec

Source: `tools/out/app.lst` (+ `tools/da.py` for the tbb-elided parts of 0x63d50 / 0x5ae6c).
[C] = read from code, [H] = hypothesis. Related: actions_macros.md §2.8, power.md §5–6, dongle_radio.md §3.

## 1. Layout and writers

RW default (RAM init): `ff 03 00 00 00 00 00 00 00 01`. Only the literal refs listed in the task touch it; no
pointer alias (0x2000277c/0x20002780 point elsewhere). The block after it (0x2000278d) is the 0x1b-byte cfg default.

| Byte | Meaning | Writers (addr → value) |
|---|---|---|
| [0] | const 0xFF | never written [C] |
| [1] | const 0x03 | never written [C] |
| [2] | current DPI stage index = 0x20007702[0] | boot 0x5a3d4; DPI buttons (dispatcher types 7/code 6..9) 0x5873a, 0x5876a, 0x587d6, 0x58806; vendor `D3` 0x52162; vendor `DF` 0x52602 (always, before the DF sub-dispatch). None of these arm [8] themselves [C] |
| [3] | power source state from 0x536c8 (0 = battery, 1..3 = external, see power.md §3) | main loop 0x5a552 when it differs from 0x536c8() (also arms, §2) [C] |
| [4] | "awake/active" flag | 0x5aea2 = 1 (activity state 2 = active, only if [4]==0 and not pairing, arms); 0x5aec2 = 0 (activity state 3 = idle); 0x5a50e = 0 (wired mode takes over, together with [8]=0) [C]. [H] this is NGENUITY's online flag - the dongle itself emits `FF 03 00 00 00` to the host when the RF link drops (dongle 0x62cc, dongle_radio.md) |
| [5] | sniper held | 0x58850 = 1 (sniper press, type 7 code 0x0B), 0x58886 = 0 (release) [C] |
| [6] | "factory reset just done" | 0x5aa24 = 1 (end of factory-reset EEPROM rewrite, event 10); 0x5ec4e = 0 after the **radio** send. USB send does not clear it [C] |
| [7] | bitmap of held physical buttons, bit = button index 0..5 (5 = DPI) | 0x58448 set on press, 0x58464 clear on release; 0x583f6 / 0x5840e = 0 on the DPI release that ends a combo (0x200024ea / 0x200024e9 latch) [C] |
| [8] | send countdown (0 = nothing pending) | see §2 |
| [9] | const 0x01 | never read or written; not transmitted [C] |

## 2. Arming ([8]) and countdown

| Trigger | Addr | Value | Gate |
|---|---|---|---|
| Physical button press **or** release, index < 6 (dispatcher prologue, runs when 0x60248 dequeues the event at a report slot) | 0x58426 | 4 | not pairing (0x2000223d≠1). Wheel/other events with index ≥ 6 do not arm [C] |
| Activity state machine 0x5ae6c enters/is in state 2 (active) with [4]==0 → [4]=1 | 0x5aea6 | 2 | not pairing [C]. Only runs in wireless branch of main loop (0x5a4ac) |
| Power state change (0x536c8 result ≠ [3]) | 0x5a55e | 2 | not pairing; every main-loop pass [C] |
| Factory reset finished (also [6]=1) | 0x5aa28 | 2 | none [C] |
| Radio host command fully received + executed (tunnel rx 0x50578 after 0x51c44) | 0x506a0 | 4 **only if [8]≠0** (re-arm/postpone, never arms from 0) [C] |
| USB HID SET_IDLE (bRequest 0x0A, class setup handler 0x61080) | 0x610be / 0x610ca | 0, then 10 if wIndex == 3 (IF3 consumer) [C] |
| Pairing active during USB slot | 0x63e78 | 0 (cancel) [C] |
| Wired mode takeover (USB configured, 0x20002262≠0) | 0x5a50c | 0 (+ [4]=0) [C] |

### 2.1 Radio countdown/send - 0x5ea44 (called from 0x509bc when TIMER1 1 ms flag 0x20002240.0 is set and not pairing) [C]
```
tick (1 ms):
  cnt++                                   // 0x20002260
  r = 0
  if cnt >= div(poll):  cnt = 0; if !(0x20002323 & 4): r = 0x60248(buf)   // HID report / dispatcher
  if r==0 && !(2323&4) && st[8]==0 && !macro_pending: idle tick (0x20002244=1) ; return
  if 2323&4: → send pending tunnel packet (see below)
  elif r==1/2/3: send 0x60/0x61/0x62 report
  else:                                   // 0x5ec12: no HID report this tick
     if st[8]==0: ...(macro) 
     st[8]--                              // 0x5ec26
     if st[8]!=0: return                  // 0x5ec52: nothing is transmitted this tick
     if pairing: goto send                // unreachable here
     0x20002325 = 1; 0x20002324 = 9       // segments, bytes_left
     0x53a5c(st, 8)                        // L=0x2000430a[2]=8, 0x20002326=1, zero 0x200043d1[64],
                                          // copy st[0..7], set 0x20002323 |= 4
     st[6] = 0                            // 0x5ec4e
  0x20002244 = 0; 0x2000225e = 0; 0x20002314 = 9999
  if 2323&4 && esb_idle(0x5c28c): 0x506f0(pkt=0x20002834, 0)   // builds E0 packet (same tick)
  if esb_idle: if P0.09 || ++0x20002256 >= 500: nrf_esb_write_payload(pkt)   // 0x5ecdc
```
- Decrement = once per 1 ms tick in which no HID/macro/tunnel packet is produced (at <1000 Hz polling also on the
  non-slot ticks). So a button edge (arm 4) is sent on the **4th empty tick** after the last report, i.e. ≈4 ms after
  the release at 1 kHz; arm 2 → 2nd empty tick. Continuous motion/reports stall the countdown.
- Suppressed while a tunnel reply is pending (0x20002323 bit 2: countdown frozen), while pairing (0x5ea44 takes the
  pairing branch, [8] stays), in wired mode (5ea44 not run; [8] cleared at 0x5a50c). If ESB is busy at 0x5ecce the
  E0 packet is simply built on a later tick (bit 2 stays set) - not lost. It is lost only if the final
  write gate at 0x5ecdc fails (P0.09==0 and 0x20002256<500, charger-related [H]).

### 2.2 E0 packet built by 0x506f0 [C]
`nrf_esb_payload_t` @0x20002834: len = bytes_left+4 = **13**, pipe = **2**, data:

| data[i] | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| value | 00 | E0 | L=08 | SEQ=01 | FF | 03 | st[2] | st[3] | st[4] | st[5] | st[6] | st[7] | 00 |

- L = 0x2000430a[2] = min(8,64) = 8; SEQ = ++0x20002333 (0 → **1**; reset to 0 when segment count reaches 0).
- 9 body bytes are copied (bytes_left=9 > L=8); the 9th is the zeroed reply buffer, not st[8]/[9].
- After the send: 0x20002324 = 0, 0x20002325 = 0, 0x20002332 = 0, 0x20002333 = 0, **0x20002323 = 0** (whole byte),
  0x20004391[64] zeroed. st[6] was already cleared at 0x5ec4e (after being copied); st[8] is 0.
- st[6] value on air is the pre-clear value (1 only for the first status after a factory reset).

### 2.3 USB countdown/send - SOF event of 0x63d50 (case 0 @0x63d70) [C]
```
if suspended (0x20002475): return
sof++ (0x20002476); if sof < (IF0 protocol==boot ? 8 : div(poll)): return
r = 0x60248(buf)
if r==0 && st[8]==0 && !macro: idle; sof=0; return
if 0x200026e0 != 0: sof=0; return                 // slot dropped, no decrement
if macro: send macro  elif r: send report
else if st[8]:                                     // 0x63e3e
    if pairing: st[8]=0
    else: st[8]--; if st[8]==0: 0x5af0c(st[0..7])
sof = 0
```
0x5af0c: if IF0 protocol byte (0x647d4+0x11) == 0 (boot) → nothing (status lost). Else 64-byte buffer 0x20005e28 =
`FF 03 st[2] st[3] st[4] st[5] st[6] st[7]` + 56 × 00, sent with 0x55cee(0x6480c = IF1 vendor, EP 0x82, 64). No
report ID. [6]/[8] are not touched afterwards. Decrement is per report slot (1 ms at 1 kHz, 8 ms at 125 Hz).

### 2.4 Path selection [C]
- Wired (0x2000246a=1, set on USB configured, event 15 @0x63eec) → only USB path (SOF); radio 0x5ea44 only runs to
  flush release packets, after [8]/[4] were zeroed (0x5a508).
- Wireless (0x2000246a=0) → radio path; 0x5ae6c state machine runs only here, so [4] is only set in wireless mode.
- USB not configured/suspended → no SOF sends. USB has no tunnel gate.

## 3. Trigger checklist (answers to Q4)

| Event | Armed? | Where |
|---|---|---|
| Boot, wireless | **Yes**, 2 (via state 0→1→2, [4]:0→1). Plus 2 more if booted on external power ([3] 0→n) | 0x5aea6, 0x5a55e |
| Boot, wired | wireless-branch arm is cleared when USB configured (0x5a50c); then SET_IDLE(IF3) arms 10 → USB status ≈10 slots later [H: depends on host sending SET_IDLE to IF3 after the clear] | 0x610ca |
| Wake from deep sleep (state 4 → 2 via 0x5aec6 when TIMER1 runs again) | **Yes**, 2, [4]=1 (was cleared in idle) | 0x5aea6 |
| Idle entry (state 3) | **No**: [4]=0 silently | 0x5aec2 |
| Radio re-init (pairing end/timeout → 0x5ae4c state 1 → 2) | only if [4]==0 | 0x5aea6 |
| Power/charger state change | Yes, 2 | 0x5a55e |
| DPI change by DPI button | Yes, via button edge (4), [2] updated | 0x58426 / 0x5873a.. |
| DPI change by host (D3 / DF) | No arm; [2] updated; over radio a pending countdown is reset to 4 (0x506a0) | 0x52162, 0x52602 |
| Every physical button press and release (index 0..5, incl. DPI, sniper) | Yes, 4 | 0x58426 |
| Wheel detents | No (index ≥ 6) [H on index] | – |
| Factory reset | Yes, 2, [6]=1 | 0x5aa28 |
| Periodic/heartbeat | **None** [C] | – |

## 4. Q5: 0x50696 and 0x52162 / 0x52602
- 0x50696/0x5069e is in the radio host→mouse tunnel receiver 0x50578: after the last chunk is assembled into
  0x20004391 and 0x51c44 (vendor command dispatcher) ran, `if st[8] != 0: st[8] = 4` - it postpones a pending status
  so the command reply (0x20002323 bit 2, set at 0x5066c) goes first. It never arms from 0 and never reads it back.
- 0x52162: `D3 00 x x s` (set DPI stage) → st[2] = s. 0x52602: `DF …` → st[2] = DPI[0]. No host command reads,
  returns or resets the block; st[8] is not touched by vendor commands.

## 5. Reimplementation notes [H]
- Keep state: `st = {FF,03,dpi,pwr,awake,sniper,frst,btn}`; countdown per 1 ms radio tick without other TX.
- Radio frame to send (pipe 2, 13 bytes): `00 E0 08 01 FF 03 dpi pwr 01 sniper 00 btn 00`, SEQ=1 each time.
- Minimum for NGENUITY "online": send with awake=1 at boot, after every wake from sleep/idle and on every button
  edge (the dongle's own link-loss notification is `FF 03 00 00 00`, i.e. awake=0).
