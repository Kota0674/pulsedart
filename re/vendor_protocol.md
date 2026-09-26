# Vendor (NGENUITY) configuration protocol: byte-exact spec of stock FW 1.1.0.8

Source: static analysis of `dump/flash.bin` (app 0x50000–0x68000), handler `0x51c44` (write side up to `0x527a2`,
read side `0x527a4`, bootloader/ID side `0x5335a`, common tail `0x534c0`), with state from `dump/eeprom.bin`.
Tags: **[C]** = CONFIRMED in code or data at the given address. **[H]** = HYPOTHESIS.
Listings: `re/tools/iul_cmd.lst` (linear, 0x51c44–0x53554) and `re/tools/out/app.lst`. The reference model
`re/tools/vendor_model.py` produces the example vectors in §9 from my EEPROM dump.

Notation: `[n]` = byte n of the 64-byte request/reply. `u16[n]` = little-endian at [n..n+1]. "echo" = reply is the
request with no field changed. All numbers are hex unless written in decimal.

---

## 1. Transport and buffers

### 1.1 USB (source = 1) [C]
- Host → device: 64-byte HID **output report** without report ID on interface 1 (EP 0x03 OUT). In Windows `WriteFile`
  this is 65 bytes with a leading report ID 0x00.
- `0x58afc` (user event handler of HID instances IF0 **and** IF1, pointer at instance+0x28: 0x647fc and 0x64834):
  - event 2 (OUT report ready, 0x58b20): `len = received size` (0x55d48), copy to **request buffer 0x20005e68**, then
    `r = cmd_handler(req=0x20005e68, out=0x20005e28, len, lenptr=NULL, source=1)` (0x58b46).
    If `r != 0`: send **64 bytes from 0x20005e28** as an IN report on EP 0x82 (`0x55cee(0x6480c, 0x20005e28, 0x40)`, 0x58b52).
  - event 3 (IN report done, 0x58b58): streaming continuation of `50 01` / `50 02` (§5.3).
- The handler always receives `len` = size of the OUT transfer (64 in practice).

### 1.2 Radio tunnel (source = 0) [C]
- Dongle ACK payload type 2 (`02 L SEG data…`) → `0x50578`: payload copied to 0x200042c7, **byte [1] forced to 0x40**,
  `data[3..len)` appended to the reassembly buffer **0x20004391** at offset 0x20002324 (overflow > 64 → abort, clear).
  Because the total is forced to 0x40, the command is complete after the **first** segment (0x5063a–0x5065a).
- Then `cmd_handler(req=0x20004391, out=0x200043d1, len=0x40, lenptr=0x20002327, source=0)` (0x50682); the
  **return value is ignored**; flag 0x20002323 bit 2 is set → response is transmitted.
- If 0x20002328 == 0 after the call, `0x5661c` (idle/activity reset) runs; `D2` and `51 00 00` set 0x20002328 = 1
  so they do not count as user activity (0x51f2c, 0x52e7e).
- Response (`0x506f0`, called from the report scheduler 0x5ecd8 when ESB is free): ESB packet
  `00 E0 40 SEQ out[k..k+n)` with `n = min(bytes_left, 0x40)`, `bytes_left` = number of request bytes that were
  received (0x20002324), SEQ = 0x20002333 incremented before each packet (first packet SEQ = 1), ESB length = n+4.
  Packets are sent until the segment count (0x20002325) reaches 0; then SEQ/offset are reset and 0x20004391 is
  zeroed (64 bytes). With the dongle's normal 67-byte ACK (64 data bytes) this is **exactly one E0 packet with 64
  bytes of `out`**. `[2]` of the E0 packet is always 0x40.
- `*lenptr` (0x20002327) receives the "reply length" value (see §2, `rl`), or 6 on error. No reader of 0x20002327
  was found; it is not used for the E0 framing. [C: single literal ref at 0x50678; H: unused]

### 1.3 Buffers, zeroing [C]
| Buffer | Size | Notes |
|---|---|---|
| USB request 0x20005e68 | 64 | OUT data copied here; the handler modifies it **in place**. Pointer copies at RW 0x2000277c and 0x20002780 both = 0x20005e68. |
| USB reply 0x20005e28 | 64 | Zeroed for `len` bytes at handler entry (`0x5047c(out,len)`, 0x51c7e). IN report always sends 64 bytes. |
| Radio request 0x20004391 | 64 | Zeroed after the response has been sent (0x50792). |
| Radio reply 0x200043d1 | 64 | Zeroed for 0x40 bytes at handler entry. |

Reply content:
- **Success**: `out[0..len) = request[0..len)` after in-place modification (0x534c4). So **unused reply bytes are the
  request's bytes**, not zeros (they are zero only if the host padded with zeros, which NGENUITY is expected to do [H]).
- **Error**: `out` stays zeroed except `out[0..2] = req[0..2]`, `out[3]=02`, `out[4]=EC`, `out[5]=err` (0x534f6–0x53516).
- Read commands additionally do `memcpy(*(0x2000277c)=0x20005e68, req, len)` before returning (e.g. 0x528d4). On USB
  this is a self-copy; on radio it copies the tunnel request into the USB request buffer (harmless, but see D2 zone 2).

---

## 2. Handler skeleton (0x51c44) [C]

```
int cmd_handler(u8 *req, u8 *out, u32 len, u8 *lenptr, u32 source)   // source is the 5th arg at [sp+0x58]
{
    ok = 1; ret = 1; rl = 2; err = 0;              // sp+0x1c, sp+0x20, sp+0x18, sp+0x10
    write32(0x40010308, 1);                        // 0x5fb88 (entry; exit does 0x5e5f0(*0x2000222d) + write32(0x40010304,1))
    if (!req || !out) return 0;                    // 0x5351c
    memset(out, 0, len);
    if ((req[0] & 0xD0) == 0xD0) {                 // D0..DF and F0..FF
        if (req[0] - 0xD0 >= 0x10) { err = 1; ok = 0; }      // F0..FF -> err 1 (0x52798)
        else switch (req[0]) { D0..DF, see §3; D7..DD -> err 1 }
    } else switch (req[0]) {                       // 0x527a4
        50,51,52,53,54,55,56,57: see §4/§5;
        07 and every other value: err = 0; ok = 0; // 0x53306 / 0x53350
    }
    // 0x5335a: A0/A1/A2/A4 (§6) are tested here for every command and may set ok = 1 again
    if (ok) {
        memcpy(out, req, len);
        if (lenptr) *lenptr = rl;
        if (*(u8*)0x20002818) { *(u8*)0x20002818 = 0; ret = 0; }   // deferred: no immediate USB reply
    } else {
        if (lenptr) *lenptr = 6;
        out[0]=req[0]; out[1]=req[1]; out[2]=req[2]; out[3]=2; out[4]=0xEC; out[5]=err;
    }
    return ret;
}
```
Error codes (`[5]`): 0 = unsupported command, 1 = bad sub-command / range, 2 = bad index, 3 = bad count, 4 = value
out of range. The exact code per check is listed below (the vendor is not consistent: e.g. D0 uses err 2 for an unknown
sub-command, D6 uses err 2 for a bad macro index).

`rl` values: default 2; D0 00/D0 01 → 5; 50 00 00 and radio 50 01/02 → 0x40; 50 00 D0 → 4; 50 00 D1, 57, A2 13 →
0x0b; A0/A1 → 7. Only relevant for the radio `lenptr`.

---

## 3. Write commands D0–DF (RAM only unless noted; persist with DE) [C]

RAM blocks: cfg 0x200076c4 (0x1b), misc 0x200076df (9), LED zones 0x200076e8 (2×0x0d), DPI 0x20007702 (0x26),
buttons 0x20007728 (8×3), macros 0x20007740 + 0x209·m (m 0..5), custom LED arrays A0 0x20008385 / A1 0x20008415 (0x90 each).
Validation is performed **in the order listed**; the first failing check produces the error reply and nothing
after it is executed (except where noted). Success reply = echo (unless stated).

### D0 - general (tbh target 0x51cb4)
| Request | Checks (in order → err) | Action | rl |
|---|---|---|---|
| `D0 00 x x p` | p ≥ 4 → err 4 | cfg[0x0e] = p (poll index: 0=125,1=250,2=500,3=1000 Hz). [2],[3] ignored. | 5 |
| `D0 01 z t n` | z ≥ 2 → err 2; t ≥ 2 → err 3; n > 0x30 → err 4 | cfg[0x19+z] = n·3 (byte length of custom array z). t is only validated. | 5 |
| `D0 02 z cnt st rgb…` | z > 1 → err 2; cnt ≥ 0x14 → err 3; st ≥ 0x30 → err 4 | copy cnt·3 bytes from [5..] to A_z[st·3 ..]. **No check of st+cnt ≤ 48**: up to 198 bytes can be written, overflowing A0 into A1 / past A1. | 2 |
| `D0 54 78 x p r` | – | cfg[0x0f] = [4] (press debounce), cfg[0x10] = [5] (release). No range check. Overridden at next boot (§8.3). | 2 |
| `D0 54 87 x e` | – | 0x2000280a = (e != 0) (allow deep sleep, see input_usb_led.md §1.5) | 2 |
| `D0 54 other…` | – | nothing (echo) | 2 |
| `D0 AA 55 D0 AA 55` | if [2..5] ≠ `55 D0 AA 55` → nothing, echo | 0x20002816 = 1 (test mode: disables idle/sleep, 0x61f30; also gates radio code at 0x51424). For zone 0 and 1: z[6]=0x64, z[0]=0, z[1]=0, z[7..12]=FF; `0x5eee0(req+2, zone, &z, 1)`; sensor write reg 0x10 = 0x00 (`0x5fe0e`, PMW3389 Config2) - per zone, i.e. twice. | 2 |
| `D0 other` | → err 2 | | |

### D1 - misc / gauge (0x51ec0)
| Request | Checks | Action | Deferred |
|---|---|---|---|
| `D1 00 x x v` | v ≥ 0x1a or v ≤ 4 → err 4 | misc[2] = v (low-battery threshold %, 5..25) | no |
| `D1 01 …` | – | state 0x20002810 = 5. Main loop (0x5ab32): 0x2000228c = 1, EEPROM **0xFE00 := 01** (30 ms), run scheduler, **NVIC_SystemReset** (0x516d8). Echo is sent immediately (before the reset). | no |
| `D1 02 …` | – | state = 6 and **deferred** (0x20002818=1). Main loop (0x5ab52): 0x20002288 = 0, EEPROM **0xFE01 := 00**, state = 2 → next pass sends the echo (§3.2). | **yes** |
| `D1 94 78 x v` | – | misc[2] = v, no range check | no |
| `D1 94 other` | – | nothing | no |
| `D1 other` | → err 1 | | |

### D2 - LED zone (0x51f24)
Request: `D2 ZZ MS c1R c1G c1B c2R c2G c2B BR SP` → zone = [1]>>4 ([1]&0xF ignored), eff = [2]>>4, sub = [2]&0xF,
colour1 = [4..6], colour2 = [7..9], brightness = [10], speed = [11]. [3] ignored. Always sets 0x20002328 = 1.
Zone record (13 bytes at 0x200076e8 + 13·zone): `[0]=eff [1]=sub [2]=speed [3..5]=untouched [6]=brightness [7..9]=c1 [10..12]=c2`.

| zone | Sequence |
|---|---|
| 0 / 1 | [10] ≥ 0x65 → err 1. z[6] = [10]. eff ≥ 5 or sub ≥ 4 → err 2 (brightness already stored). z[0]=eff, z[1]=sub, z[2]=[11], z[7..12]=[4..9]; `0x5eee0(req+2, zone, &z, 1)` (apply). |
| 2 (both) | Brightness is read from **`*(0x20002780)`[10] = 0x20005e68[10]** (the USB request buffer), ≥ 0x65 → err 1. Then for each zone 0,1: z[6] = [10] (from the request); if eff/sub invalid → err 2 and **continue with the next zone**; else z[0],z[2],z[1],z[7..12] as above. After the loop `0x5eee0(req+2, 2, 0x200076e8, 1)` is **always** called, even after err 2. Over USB 0x20005e68 is the request, so this is correct; over radio the check uses a stale byte (stock bug). |
| ≥ 3 | err 1 |

### D3 - DPI (0x5212e)
DPI block (0x20007702): `[0]=current stage [1]=01? [2]=#enabled [3..4]=min(2) [5..6]=max(320) [7]=enable mask [8..9]=sniper
[0x0a..0x13]=5×u16 DPI (unit 50 CPI) [0x14..0x16]=unused [0x17..0x25]=5×RGB`. Stage list 0x2000250e[5], count 0x20002514,
list index of current stage 0x20002513.

| Request | Checks | Action |
|---|---|---|
| `D3 00 x x s` | s ≥ 5 → err 4; mask bit s clear → err 4 | DPI[0]=s; sensor DPI = DPI[0x0a+2s] (`0x6097c`); 0x20002784[2]=s; 0x20002513 = index of s in the list |
| `D3 01 00 x m` | [2] ≠ 0 → err 2; m > 0x1f → err 4 | count=0, list[0..4]=0, DPI[7]=m, rebuild list (bit order 0..4), 0x20002513=0, DPI[0]=list[0], DPI[2]=count, apply DPI of list[0]. **m = 0 is accepted** (empty list, stage 0). |
| `D3 02 s x lo hi` | s ≥ 5 → err 2; mask bit s clear → err 2; v=u16[4] < 2 or > 0x140 → err 4 | DPI[0x0a+2s] = v; if s == DPI[0] apply to sensor |
| `D3 03 s x R G B` | s ≥ 5 → err 2; bit s clear → err 2 | DPI[0x17+3s..] = [4..6] |
| `D3 04 00 x lo hi` | [2] ≠ 0 → err 2 | if 2 ≤ v ≤ 0x140: DPI[8..9] = v; **else silently ignored (echo, no error)** |
| `D3 ≥05` | err 1 | |

### D4 - button map (0x52306)
`D4 b t x c`: b ≥ 6 → err 1; t ≥ 8 → err 2. map[b] = {t, c, aux = 0x50c04(t,c)} (aux: t=1 → 1<<(c−1), t=3 → 1<<c,
else c). [3] ignored. Entries 6/7 (wheel) cannot be written.

### D5 - macro header (0x52562)
`D5 m 00 x lenlo lenhi mode rep`:
[2] ≠ 0 → err 2; m>>4 ≠ 0 → err 1; (m&0xF) ≥ 6 → err 1; u16[4] > 0x200 → err 4; ([7] > 1000 - never true, byte);
[6] > 3 → err 4. Then macro[0..1] = u16[4], macro[6] = [6], macro[7..8] = (mode == 1) ? **[7] zero-extended** : 0.
Event counters macro[2..5] are **not** reset here.

### D6 - macro data (0x52354)
`D6 m a b ev…` with n = b & 0x3F (events in this packet), o = (a<<2) | (b>>6) (events to skip):
m>>4 ≠ 0 → err 2; (m&0xF) ≥ 6 → err 2; o ≥ 0x200 or n > 6 → err 1.
Action: macro[2]=macro[4]=0 (u16 counters of 5-byte and 10-byte events); walk o events from data start (macro+9),
each 10 bytes if its first byte is 0x1A else 5, incrementing the counters; then copy n events from req[4..] to the
current position with the same length rule (0x1A → 10 bytes, counter at [4]; else 5 bytes, counter at [2]).
No bound check against 0x200 data bytes.

### D7–DD → err 1 (0x52798)

### DE - save to EEPROM (0x5276e)
`DE k`: k == 0xFF or k < 6 → 0x20002817 = k, **deferred** (0x20002818 = 1), state 0x20002810 = 4. Else err 1.
Note k is at [1] (DF uses [2]). See §3.2 and §7.

### DF - load defaults (0x525fe), RAM only (does NOT write EEPROM)
Always first: 0x20002784[2] = DPI[0]. `DF AA k`: [1] ≠ 0xAA → err 1.

| k | Action |
|---|---|
| 0 | cfg = RW defaults 0x2000278d (0x1b bytes, **including debounce 03/12 and idle 10000 - the boot overrides of §8.3 are not re-applied**); A0, A1 zeroed (0x90 each) |
| 1 | misc[2] = default misc[2] (0x0f) only |
| 2 | LED zones = 0x200027b1 (0x1a); `0x606ec(1)` (re-apply) |
| 3 | DPI = 0x200027cb (0x26); apply sensor DPI[0x0a+2·DPI[0]]; `0x50aa4` (rebuild stage list) |
| 4 | buttons = 0x200027f1 (0x18) |
| 5 | for all 6 macros: len = cnt5 = cnt10 = 0 (mode/repeat/data untouched) |
| FF | misc[2], LED(+apply), DPI (apply sensor with **DPI[0x0a]**, i.e. stage-0 value), `0x50aa4`, buttons, cfg, A0/A1 zeroed, macros as k=5 |
| other | err 2 |

### 3.2 Deferred replies (0x20002818) and the state machine 0x20002810 [C]
Commands that set 0x20002818: `D1 02`, `DE k`, `50 00 D2`, `A4 B0 C1 EA …` (USB only).
- USB: handler returns 0 → no IN report now. The echo is already in 0x20005e28. The main loop (`0x5aa80`, tbb at 0x5aa8e)
  later runs the action and then state **2** → `0x51b44` sends 64 bytes of 0x20005e28 on EP 0x82. If another command
  arrives before that, 0x20005e28 is overwritten and the late reply carries the newer content [C by code, race H].
- Radio: the return value is ignored, the echo goes out immediately as E0; the later `0x51b44` send goes to the
  (unconfigured) USB stack [C/H].

| state | Set by | Main-loop action |
|---|---|---|
| 0 | – | none |
| 1 | A4 | 0x2000223b = 3; `0x50d10` (save pairing); → 2 |
| 2 | – | → 0; send 0x20005e28 (64 B) |
| 3 | 50 00 D2 | if pairing[6]==1: 0x20002248 = 1; EEPROM 0x1690 := `01 00 00 00`; EEPROM 0x1490 := 0x20002232[0..7] (8 B); EEPROM 0x16A0 := 01; → 2 |
| 4 | DE | → 2 (so **the reply is sent on the next loop pass, before the data is written**); 0x2000225b = 1 (save pending), 0x20002244 = 0, 0x2000225c = (k==FF ? 0 : k) |
| 5 | D1 01 | 0x2000228c = 1, EEPROM 0xFE00 := 01, → 0, scheduler, reset |
| 6 | D1 02 | → 2, 0x20002288 = 0, EEPROM 0xFE01 := 00 |

Save gate (0x5ab7a): `0x2000225b && 0x20002244 && u16 0x2000225e >= 100` → `0x53b64(k)`. 0x20002244 = "no report to
send this tick", 0x2000225e = consecutive idle report ticks (set/incremented at 0x5eb98, cleared at 0x5ec5a when a
report is built). So the EEPROM write starts after ≈100 ms without motion/buttons [H for the ms scale].

---

## 4. Read / info commands [C]

Reply = request with the listed bytes overwritten (§1.3). "req" in a cell = byte left as sent by the host.

| Request | Checks | Reply fields | rl |
|---|---|---|---|
| `50 00 00` | – | [3]=1D, [4..11]=`E2 16 51 09 08 00 01 01` (PID LE, VID LE, FW 0x01010008 LE), [12..33]=`"HyperX Pulsefire Dart"` + 00 (strcpy from 0x529e4) | 40 |
| `50 00 01` | – | [3]=**req** (not set), [4..11]=`02 0E 01 05 08 00 01 01` | 2 |
| `50 00 D0` | – | [3]=01, [4]=EEPROM[0x1690] (blocking I²C read of 4 bytes, low byte returned) | 4 |
| `50 00 D1 n` | none on n | [3]=08, [4..10]=EEPROM[0x1490+8n .. +6] (**7 of 8 bytes**; [11]=req) | 0B |
| `50 00 D2` | – | echo, deferred (state 3) | 2 |
| `50 00 other` | → err 2 | | |
| `50 01 …` / `50 02 …` | – | custom-LED stream, §5 ([2] not checked) | |
| `50 03 …` | [2] not checked | [3]=31, [4]=zone0 eff, [5]=zone1 eff, [6]=DPI[2] (#enabled), [7..8]=sniper, [9..18]=5×u16 DPI, [19..33]=5×RGB, [34..51]=6×3 button map (entries 0..5), [52]=cfg[0x0e] | 2 |
| `50 other` | → err 1 | | |
| `51 00 00` | [2]≠0 → err 2 | [3]=03 (although 10 bytes follow), [4..6]=misc[0..2], [7..8]=misc[6..7], [9]=misc[8], [10..13]=u32 0x20002288 (gauge INITCOMP counter, EEPROM 0xFE01). Sets 0x20002328=1. | 2 |
| `51 01 0 x r` | – | r8 = `0x57f40(r)` (gauge read16 register r) | 2 |
| `51 01 1 x s` | – | r8 = `0x57f54(s)` (gauge Control(sub-command) s) | 2 |
| `51 01 ≥2` | → err 2 (reply is then the error frame) | | |
| - `51 01` reply | | [3]=00, [4]=req (r), [5..8]=u32 LE result | |
| `51 ≥02` | → err 1 | | |
| `52 00 00` | [1]≠0 → err 1; [2]≠0 → err 2 | [3]=11; zone0: [4]=eff [5]=sub [6]=speed [7]=brightness [8..10]=c1 [11..13]=c2; zone1: [14]=eff [15]=sub [16]=speed [17]=brightness [18..20]=c1 [21..23]=c2 | 2 |
| `53 00 00` | [2]≠0 → err 2 | [3]=21, [4]=DPI[0] stage, [5]=DPI[7] mask, [6..7]=min, [8..9]=max, [10..11]=sniper, [12..21]=5×u16 DPI, [22..36]=5×RGB | 2 |
| `53 94 87 x r` | [2]≠0x87 → echo | [5] = sensor register r (`0x5fd52`); then if r ≠ 0x50: sensor write reg 0x50 := 01 (`0x539ac`) | 2 |
| `53 other` | → err 1 | | |
| `54 00 00` | [1]≠0 → err 1; [2]≠0 → err 2 | [3]=0C, [4..21] = **button entry 0 repeated 6 times** (stock bug at 0x53128: the loop never indexes the table). Use `50 03` [34..51] for the real map. | 2 |
| `55 m` | m>>4 ≠ 0 → err 1 | index = **m & 0xF0 (= 0)**: always macro 0. [3]=req, [4..5]=len, [6]=mode, [7..8]=repeat. No memcpy to 0x2000277c. | 2 |
| `56 m a b` | m>>4 ≠ 0 → err 1; n=b&0x3F > 6 → err 2; o=(a<<4)\|(b>>6) ≥ 0x200 → err 2 | index = m & 0xF0 (= 0). Walk events of macro 0 (5/10 bytes by 0x1A rule) while byte offset < 0x200; at event #o copy n events to [4..]. [3]=req. Note **a<<4 here vs a<<2 in D6**. | 2 |
| `57 13 01 00` | any other [1..3] → err 0 | [3]=08, [4..10]=pairing record 0x20002232[0..6] (`CH 02 a0 a1 a2 a3 01`) | 0B |
| `07` and any unknown cmd | | error frame `cc ss ii 02 EC 00` | |

---

## 5. Custom LED array streaming: `50 01` (array A0, length cfg[0x19]) / `50 02` (A1, length cfg[0x1a]) [C]

Variables: 0x20002814 = active stream (1/2, 0 = none), 0x20002813 = byte offset (u8). Chunk = 60 bytes = 20 RGB entries.

### 5.1 USB (source = 1), first reply (0x528e2 / 0x52b54)
```
0x2814 = z (1|2); req[0]=0x50; req[1]=z; req[2]=0;
T = cfg[0x18+z]                                  // total bytes (entries*3)
if (T > off) {                                   // off = 0x2813, normally 0
    if (T < 60) { req[3] = T/3; copy T bytes from A+off to req[4..]; 0x2814 = 0; off = 0; }   // note: T, not T-off
    else        { req[3] = 20;  copy 60 bytes from A+off;            off += 60; }             // stream stays active
} else          { 0x2814 = 0; off = 0; }          // req[3] left as sent
```
Reply = that buffer (normal IN report).

### 5.2 USB continuation (IN-report-done event, 0x58b58) - no host request needed
Fires on **every** IN completion of IF0 (mouse) or IF1 (vendor), both use handler 0x58afc. If 0x2814 ≠ 0:
```
p = zeros(64); p[0]=0x50; p[1]=z;
if (T > off) {
    p[2] = (z==1) ? off/3 : off;                 // z==2 reports the raw byte offset (stock inconsistency)
    rem = T - off;
    if (rem < 60) { copy rem bytes; p[3] = rem/3; 0x2814 = 0; off = 0; }
    else          { copy 60 bytes;  p[3] = 20;   off += 60; }
} else { 0x2814 = 0; off = 0; }                  // p = 50 z 00 00 00… : terminator
send 64 bytes of p (from 0x20005e28) on EP 0x82
```
Consequence: if T is exactly 60 or 120 bytes, one extra all-zero-data packet `50 z 00 00 …` ends the stream.
Continuation packets are fully zero-padded (unlike normal replies).

### 5.3 Radio (source = 0): host/dongle-driven, `AC EC 01` / `AC EC 04` (0x5296a / 0x52bdc)
State at 0x200076b2: `[0]` pending, `u16[1]` total, `u16[3]` offset, `u16[5]` remaining, `[0x0a..0x0c]` saved header
(50, z, 00), `[0x0d]` entries in last chunk, `u32[0x0e]` array pointer.
- Request `50 z …` with `[4..6] ≠ AC EC 01` (first request): header saved; if T < 60: [3]=T/3, copy T bytes from
  A+0x2813, remaining=0, 0x2814=off=0. Else [3]=20, copy 60 bytes, 0x2813 += 60, pending=1, [0x0d]=20, total=T,
  offset=60, remaining=T−60, ptr=A (0x2814 stays z). rl = 2.
- Request `50 z' x x AC EC 01` (continuation; z' may be 1 or 2, state is shared): [0..2] = saved header
  (`50 z 00`), rl = 0x40, then
  - remaining == 0: [3]=00, [4..6]=`AC EC 04`, 0x2813 = 0x2814 = 0 (end marker);
  - remaining < 60: [0x0d]=remaining/3, copy remaining bytes from ptr+offset, remaining=0, [3]=[0x0d];
  - else: [0x0d]=20, copy 60 bytes from ptr+offset, offset += 60, remaining = total−offset, [3]=20.
- The stock dongle re-issues `50 …` requests until it sees `AC EC 04` (dongle_radio.md §3, 0x561e) [C on dongle side].
- For T = 0 the first reply has [3]=00 and **no** end marker; the end marker only comes on the next `AC EC 01`.

---

## 6. Bootloader / ID commands (0x5335a), tested after every other path [C]
| Request | Reply | Notes |
|---|---|---|
| `A0 EA 5A A5` | `A0 EA 02 00 EC AC …` | unlock (0x20002811 = 1). rl 7 |
| `A0 DA 5A A5` | `A0 DA 02 00 EC AC …` | lock |
| `A0` other | `A0 ss 02 00 EC FE …` | always ok frame |
| `A1 00 4B B4 94 10 98 27 24 10 00 01` (unlocked) | `A1 00 02 00 EC AC` | calls `0x56cd8` (enter DFU; resets, reply likely never sent) |
| `A1` other | `A1 ss 02 00 EC FE` | |
| `A2 10` (unlocked) | `A2 10 04 00 65 09 00 02` | u32 at flash 0x50200 |
| `A2 13 01 00` (unlocked) | `A2 13 08 00 req req req req CH 02 a0 a1 a2 a3 01` | ID at [8..14]; [4..7] untouched; rl 0x0B |
| `A2 13` other (unlocked) | err 2 | |
| `A2` other sub (unlocked) | err 1 | |
| `A2 …` locked | err 0 | |
| `A4 B0 C1 EA i0..i6` (unlocked, source = 1) | echo, deferred (state 1) | 0x20002232[0..6] = [4..10], then save pairing |
| `A4` otherwise | err 0 | |

---

## 7. DE save: EEPROM writes per block (0x53b64) [C]

EEPROM access `0x50c40(addr16, buf, len, write)` (I²C addr 0x50, TWI1): **one I²C transaction per call**
(`addrH addrL data…`), no page splitting in software, then a **30 ms wait** (timer 0x200022a8 = 30 with the scheduler
running if 0x20002283 ≠ 0, else 30 × `delay_us(1000)`). After every write the scheduler (`0x54e14`) runs. No checksum,
no version bytes are written by DE.

| k (0x2000225c) | Writes (address : length) |
|---|---|
| 0 | cfg 0x0290 : 0x1B; A0 0x0490 : 0x40, 0x04D0 : 0x40, 0x0510 : 0x10; A1 0x0690 : 0x40, 0x06D0 : 0x40, 0x0710 : 0x10 |
| 1 | misc 0x0060 : 9 |
| 2 | LED 0x0110 : 0x1A |
| 3 | DPI 0x0090 : 0x26 |
| 4 | buttons 0x0190 : 0x18 |
| 5 | for m = 0..5: header 0x0210+0x10·m : 9 (bytes 0..8 of the macro); data L = 5·cnt5 + 10·cnt10 bytes at 0x0890+0x200·m in 16-byte writes (0x0890+0x200m+0x10j), last write L mod 16 |
| FF | 0,1,2,3,4,5 in order, **one block per gate opening** (0x2000225c++ after each; 0x2000225b cleared after block 5) |

For k ≠ FF, 0x2000225b is cleared after the single block.

**Page-wrap hazard [H]:** the part is 64 KB (24x512 class, 128-byte pages, see eeprom.md/power.md). The writes
0x04D0:0x40 and 0x06D0:0x40 cross the 0x0500 / 0x0700 page boundaries, so on a 128-byte-page part bytes 0x30..0x3F of
those writes (array bytes 0x70..0x7F = entries 37..42) wrap to 0x0480..0x048F / 0x0680..0x068F instead of
0x0500..0x050F / 0x0700..0x070F. The boot loader reads 0x0490:0x70 + 0x0500:0x20, so those entries would read back
wrong. All other stock writes stay inside one 128-byte page. A new implementation should split writes at page
boundaries (behaviour then differs from stock only in the buggy case).

---

## 8. EEPROM boot load (0x59f14 → 0x59f68 …) [C]

### 8.1 Sequence
1. `0x53ad4`: RAM = RW defaults (cfg, misc, LED, DPI, buttons), macros = zeros (0xC36 bytes from bss 0x20006a7c), A0/A1 = 0.
2. Read EEPROM 0xFE00 (1) → 0x2000228c.
3. Read 0x0010 (4 bytes) → `v = b0 | b1<<4 | b2<<8 | b3<<12`; compare with 0x2000227c = **0x4707**
   (EEPROM bytes `07 00 07 04`).
4. **Mismatch** (0x59fae): 0x20002283 = 0 (busy-wait delays), then write the RAM defaults:
   0x0060:9, 0x0020:1 (poll toggle 0x20002809 = 00), 0x0090:0x26, 0x0110:0x1A, 0x0190:0x18, 0x0290:0x1B,
   0x0490:0x70, 0x0500:0x20, 0x0690:0x70, 0x0700:0x20 (all zero), 0x0210+0x10m:9 for m=0..5 (zeros; macro data not
   touched), FW version at **0x0001 = `fw&F, (fw>>4)&F, (fw>>8)&F, (fw>>12)&FF` = `08 00 00 10`** (encoding bug,
   repaired on the next boot by step 5), layout version at **0x0010 = `07 00 07 04`**.
5. **Match** (0x5a0e4): read 0x0020:1 → 0x20002809, 0x0060:9, 0x0090:0x26, 0x0110:0x1A, 0x0190:0x18, 0x0290:0x1B,
   0x0490:0x70, 0x0500:0x20, 0x0690:0x70, 0x0700:0x20; macros (`0x51258`): header 0x0210+0x10m:9, data at
   0x0890+0x200m in 0x70-byte reads, L = 5·cnt5+10·cnt10. Then read 0x0001:4 as LE u32 `fw`; if fw ≤ 0x01010002 or
   fw == 0xFFFF → 0x20002248 = 0 (overwritten again in step 6); if fw ≠ 0x01010008 → write 0x0001 =
   `fw&F, (fw>>8)&F, (fw>>16)&F, fw>>24` = **`08 00 01 01`**.
   No range validation of loaded values was found.
6. Both paths: read 0x1690:4 → 0x20002248 (pairing count); read 0x16A0:1; if ≠ 1: read flash 0xEF000 (8 B) →
   0x20002232; if its byte 6 == 1: write 0x1490:8 = record, 0x1690:4 = `01 00 00 00`, 0x16A0:1 = 01.

### 8.2 Byte formats
- 0x0001 FW version: effectively LE u32 1.1.0.8 = `08 00 01 01` (as in my dump).
- 0x0010 layout version: 4 nibbles `07 00 07 04` = 0x4707. Changing the RAM constant 0x2000227c forces a rewrite.

### 8.3 RAM overrides after load (0x5a276) - not written back
cfg[0x0f] = **5** (press debounce), cfg[0x10] = **0x12 = 18** (release), u32 cfg[0x11..0x14] = **0x2904 = 10500**
(idle stage 1, ms), u32 cfg[0x15..0x18] = **0xEA60 = 60000** (stage 2). If 0x20002809 ≠ 0 → 0x20002809 = 3.
Then 0xFE01:1 → 0x20002288 (gauge wait counter, 0x5a2c6; later gauge logic may update it, power.md).

---

## 9. Test vectors (my EEPROM, USB, zero-padded requests) [C via `re/tools/vendor_model.py`]
Replies are 64 bytes; trailing zeros abbreviated.
```
50 00 00    -> 50 00 00 1d e2 16 51 09 08 00 01 01 48 79 70 65 72 58 20 50 75 6c 73 65 66 69 72 65 20 44 61 72 74 00 …
50 00 01    -> 50 00 01 00 02 0e 01 05 08 00 01 01 …
50 03       -> 50 03 00 31 01 01 03 10 00 10 00 20 00 40 00 80 00 40 01 00 00 ff ff ff 00 00 ff 00 ff 00 00 ff c0 cb
               01 01 01 01 02 02 01 03 04 01 04 08 01 05 10 07 08 00 03 …
51 00 00    -> 51 00 00 03 00 00 0f 00 00 00 40 00 00 00 …        ([10..13] = EEPROM 0xFE01 after boot; H)
52 00 00    -> 52 00 00 11 01 02 20 32 ff ff ff 00 00 00 01 02 20 32 ff ff ff 00 00 00 …
53 00 00    -> 53 00 00 21 02 07 02 00 40 01 10 00 10 00 20 00 40 00 80 00 40 01 00 00 ff ff ff 00 00 ff 00 ff 00 00 ff c0 cb …
54 00 00    -> 54 00 00 0c 01 01 01 (×6) …                           (stock bug)
57 13 01 00 -> 57 13 01 08 02 02 6e 2f 90 c4 01 …
50 00 D1 00 -> 50 00 d1 08 02 02 6e 2f 15 41 01 …                   (EEPROM 0x1490)
07          -> 07 00 00 02 ec 00 …
D0 00 00 00 07 -> D0 00 00 02 EC 04 …          D4 06 … -> D4 06 00 02 EC 01 …     D9 … -> D9 xx xx 02 EC 01
D0 00 00 00 02 -> (echo) D0 00 00 00 02 …      DE 00   -> no IN report now; echo `DE 00 …` on a later main-loop pass
```

---

## 10. USB descriptors (from flash) [C unless marked]

### 10.1 Device (0x66d90)
`12 01 00 02 00 00 00 40 51 09 E2 16 08 11 01 02 03 01` - USB 2.00, class 0/0/0, EP0 64, VID 0x0951, PID 0x16E2,
bcdDevice 0x1108, iManufacturer 1, iProduct 2, iSerial 3, 1 configuration.

### 10.2 Configuration (header 0x66da2, assembled by app_usbd at 0x60c18)
```
09 02 74 00 04 01 00 E0 FA                         wTotalLength 0x74 (116), 4 interfaces, value 1, iConfig 0,
                                                    bmAttributes 0xC0 in flash, |0x20 remote-wakeup when any class
                                                    registered RWU (0x60c26; each HID instance does on append, 0x55c84) [H: 0xE0 on wire], 250×2 mA
-- IF0 mouse (instance 0x647d4)
09 04 00 00 01 03 01 02 00
09 21 11 01 00 01 22 40 00                          HID 1.11, country 0, 1 report descriptor of 0x40 bytes
07 05 81 03 40 00 01                                EP1 IN interrupt, 64, bInterval 1
-- IF1 vendor (instance 0x6480c)
09 04 01 00 02 03 00 00 00
09 21 11 01 00 01 22 19 00
07 05 82 03 40 00 01                                EP2 IN
07 05 03 03 40 00 01                                EP3 OUT
-- IF2 keyboard (instance 0x64844)
09 04 02 00 01 03 00 01 00
09 21 11 01 00 01 22 2F 00
07 05 84 03 40 00 01
-- IF3 consumer (instance 0x6487c)
09 04 03 00 01 03 00 00 00
09 21 11 01 00 01 22 17 00
07 05 85 03 40 00 01
```
Field sources: interface descriptor writer 0x58dca (bLength 9, type 4, number, alt 0, bNumEndpoints, class 3,
subclass = inst[0x18], protocol = inst[0x19], iInterface 0); HID descriptor 0x5905a (0x21, bcdHID 0x0111, country 0,
count, 0x22, length); endpoint 0x5923c (`07 05 addr 03 40 00 01`: bInterval hard-coded 1). Subclass/protocol bytes in
the instances: IF0 `01 02`, IF1 `00 00`, IF2 `00 01`, IF3 `00 00`. The byte order of the total above assumes the
standard app_usbd order (interface, HID, endpoints ascending) [H for order, C for the pieces].

### 10.3 Report descriptors
- IF0 mouse (0x646f8, 64 B): `05 01 09 02 a1 01 09 01 a1 00 05 09 19 01 29 05 15 00 25 01 75 01 95 05 81 02 75 01 95 03 81 01 75 10 95 02 05 01 09 30 09 31 16 01 80 26 ff 7f 81 06 09 38 15 81 25 7f 95 01 75 08 81 06 c0 c0`
- IF1 vendor (0x64744, 25 B): `06 13 ff 09 01 a1 01 15 00 26 ff 00 75 08 95 40 09 02 81 02 09 03 91 02 c0`
- IF2 keyboard (0x6476c, 47 B): `05 01 09 06 a1 01 05 07 19 e0 29 e7 15 00 25 01 75 01 95 08 81 02 95 01 75 08 81 01 95 06 75 08 15 00 26 fb 00 05 07 19 00 2a fb 00 81 00 c0`
- IF3 consumer (0x647a8, 23 B): `05 0c 09 01 a1 01 95 01 75 10 15 01 26 ff 02 19 01 2a ff 02 81 00 c0`

### 10.4 Strings (0x66de0; RAM copy 0x200026e4)
- 0: `04 03 09 04` (LANGID 0x0409)
- 1: `12 03` "Kingston"
- 2: `2C 03` "HyperX Pulsefire Dart"
- 3: `1A 03` "000000000000" (constant serial)
- 4: `0E 03` "User 1" (not referenced by the device/config descriptors) [C data, use unknown]

### 10.5 Other USB behaviour relevant to NGENUITY
- Mouse report is sent from SOF at 1000/500/250/125 Hz per cfg[0x0e] (input_usb_led.md §3.4).
- HID class requests (GET/SET_REPORT, SET_IDLE, SET_PROTOCOL) are the nRF5 SDK defaults. [H] A SET_REPORT(Output) on
  EP0 to IF1 also raises the OUT_REPORT_READY event in the SDK, so implement both interrupt-OUT and control SET_REPORT.

---

## 11. Implementation notes for a 1:1 clone
- Emulate: in-place echo with request padding, error frame zeroing, rl/`lenptr` only for the tunnel, deferred reply for
  D1 02 / DE / 50 00 D2 / A4, immediate reply for D1 01 before reset.
- Stock bugs a host may already work around (keep unless NGENUITY is proven not to depend on them): `54 00 00` repeats
  entry 0; `55`/`56` only address macro 0; `56` offset uses a<<4 vs D6 a<<2; `50 00 01` leaves [3]; `50 00 D1` returns
  7 of 8 bytes; `50 02` continuation [2] = byte offset; D3 04 out-of-range silently ignored; D2 zone 2 on radio.
- Unsafe stock behaviour worth fixing (no host-visible change in normal use): D0 02 / D6 buffer overruns, DE page wrap.

## 12. Open questions
1. Whether NGENUITY ever sends non-zero padding (affects the "unused bytes = request bytes" rule only cosmetically).
2. Meaning of `50 00 01` payload `02 0E 01 05`, DPI[1] (=01), zone bytes [3..5], cfg[0x00..0x0d], misc[0,1,6..8].
3. Whether any NGENUITY version depends on `rl`/0x20002327 (no reader found in the mouse).
4. Macro event byte format and playback (player not traced).
5. On-wire bmAttributes (0xC0 vs 0xE0) and descriptor order: confirm with a USB capture of the stock mouse.
6. Exact semantics of the save gate (0x20002244 / 0x2000225e in wired mode).
7. Newer NGENUITY builds may look for HP VID 0x03F0 products; this firmware only presents 0951:16E2.
