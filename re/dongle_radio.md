# 2.4 GHz link, dongle side (nRF52810, ESB PRX): stock firmware analysis

Source: static analysis of `dump/dongle/flash.bin` (+ `ficr.bin`). Tags: **[C]** CONFIRMED (seen in code/data),
**[H]** HYPOTHESIS (inferred), **[?]** unknown. All addresses are flash offsets unless they start with 0x2000xxxx (RAM)
or 0x4000xxxx (peripheral). The mouse side is in `re/radio.md`; "mouse" addresses below refer to that file.

Helper scripts (new, `re/tools/dongle/`): `ddis.py` (disassembler with literal annotation; `ddis.py all` writes
`stub.lst` / `app.lst`), `dxref.py` (literal-pool xref), `dcallers.py` (BL/pointer xrefs), `funcs.py` → `funcs.txt`
(function map with literals/callees), `dscatter.py` (RW-data decoder → `ram_app.bin`, `ram_stub.bin`), `airaddr.py`
(record → BASE/PREFIX register values and on-air address).

---

## 0. Link spec (implement directly)

### 0.1 Radio registers (dongle, PRX)

The dongle runs the **same Nordic nRF5 SDK `nrf_esb.c`** as the mouse (same function bodies, same Primax
additions), configured as **PRX, ESB_DPL**. Values below are what the dongle writes. The last column compares them
with the mouse (`radio.md` §0/§2).

| Register | Dongle value | Where | vs mouse | Tag |
|---|---|---|---|---|
| MODE | **0** (Nrf_1Mbit) = cfg.bitrate | 0xa408 | same | C |
| MODECNF0 | `|= 1` (RU = fast ramp-up), because cfg[0x15]==0x28 | 0x8dde | same | C |
| TXPOWER | **0x04 (+4 dBm)** if settings[6]==1 and not pairing, else **0xEC (−20 dBm)** | 0x845a → cfg[0xa] → 0xa504 | **differs**: mouse uses 0 dBm when paired | C |
| PCNF0 | **0x00030008** (LFLEN=8, S0LEN=0, S1LEN=3) | 0xa548 | same | C |
| PCNF1 | **0x01040044** (MAXLEN=68, STATLEN=0, BALEN=4, ENDIAN=Big, WHITEEN=0) | 0xa548 (addr_len=5 from RW init) | same | C |
| CRCCNF / CRCPOLY / CRCINIT | **2** / **0x11021** / **0xFFFF** (16-bit, address included) | 0xa450 | same | C |
| BASE0 | **0x77777777** (base0 = `EE EE EE EE`) | 0x84a4 → 0xa3b4 | same | C |
| BASE1 (paired, this dump) | **0x52893CEB** (record `4a 91 3c d7`) | 0x8596 | same formula (mouse dump has 0x76F40923 for its own record) | C |
| BASE1 (pairing) | **0x8C4CCC2C** (`31 32 33 34`) | 0x84d2 | same | C |
| BASE1 (unpaired) | **0xBBBBBBBB** (`DD DD DD DD`) | 0x85be | **differs**: mouse uses `CC CC CC CC` (0x33333333), so unpaired devices never link | C |
| PREFIX0 / PREFIX1 | **0x23404380 / 0x13E363A3** (prefixes `01 C2 02 C4 / C5 C6 C7 C8`) | update_prefix(0,0x01) 0x84b8, (2,0x02) 0x84e6/0x85aa/0x85e2 | same | C |
| RXADDRESSES | **0xFF** (all 8 pipes; `rx_pipes_enabled` RW init 0x2000007e=0xFF, never changed) | 0x8ff4 | mouse: 1<<2 during its own RX | C |
| TXADDRESS | = RXMATCH of the received packet (ACK goes back on the same pipe) | 0x9704 | n/a | C |
| SHORTS | RX: **0x117** (READY_START, END_DISABLE, DISABLED_TXEN, ADDRESS_RSSISTART, DISABLED_RSSISTOP). While the ACK is sent: **0x11B** (…DISABLED_RXEN instead of TXEN). Base value 0x113 = RW init 0x20000098 | 0x8fda, 0x9610, 0x9790 | mouse TX uses 0x11B, its RX 0x113 | C |
| INTENSET | 0x10 (DISABLED only) | 0x8fe6 | mouse 0x11 | C |
| FREQUENCY | data channel (2400+ch MHz). Paired: settings[0] (=2 in dump). Unpaired: 100. Pairing: see §0.4 | 0x8f88/0x9000 | same values | C |
| TIFS | **not written** (reset value). Turnaround is ramp-up limited | no 0x40001544 literal | same | C (timing H) |
| DATAWHITEIV | **not written**; whitening off (WHITEEN=0) | no 0x40001554 literal | same | C |
| PACKETPTR | RX 0x2000106f, ACK TX 0x20001028 | 0x9006, 0x9718 | – | C |
| NVIC | RADIO prio = cfg[0x11]=**1**, SWI0 (ESB event) prio = cfg[0x12]=**2** | 0x8e10 | mouse 6/7 | C |

ESB config literal @0xa908 (24 bytes): `01 00 00 00 | 00 00 00 00 | 00 02 00 00 | 68 00 03 00 | 00 01 02 20 | 00 28 00 00`.
Overrides in 0x83fc: mode (+1) = **1 = PRX**, event_handler = 0x5a25, tx_power as above. **[C]**
Packet on air (both directions): `[preamble 1 B][address 5 B][LEN 8 bit][S1 3 bit = PID(2)|NO_ACK(1)][payload 0..68 B][CRC16]`.
Not nRF24L01+ compatible (8-bit length). **[C]**

### 0.2 Address mapping (formula)

Both sides store 4 "base" bytes `a0 a1 a2 a3` (settings bytes 2..5) and use prefix `0x02` on logical pipe 2.
Both sides compute the registers with identical code **[C]**:
- `addr_conv` (dongle 0x7828, mouse 0x54c60) = `REV(REV(RBIT(w)))` = **`RBIT(w)`**, where `w = a0 | a1<<8 | a2<<16 | a3<<24`
  → `BASE1 = bitrev8(a3) | bitrev8(a2)<<8 | bitrev8(a1)<<16 | bitrev8(a0)<<24`.
- `bytewise_bitswap` (dongle 0x7ab4, mouse 0x565a0) = `REV(RBIT(w))` = bit-reverse each byte in place
  → PREFIX0 byte 2 = bitrev8(0x02) = 0x40.

On air (nRF52: base address low byte first, then prefix, every address byte LSbit first) this becomes, as
MSbit-first bytes in transmit order: **`a3 a2 a1 a0 02`**. **[C for registers, H for on-air order (datasheet rule, not captured)]**

| Record bytes 2..5 | BASE1 | Air address (pipe 2) |
|---|---|---|
| `4a 91 3c d7` (this dongle) | 0x52893CEB | `D7 3C 91 4A 02` |
| `6e 2f 90 c4` (the mouse's record) | 0x76F40923 | `C4 90 2F 6E 02` |
| `31 32 33 34` (pairing) | 0x8C4CCC2C | `34 33 32 31 02` |
| `DD DD DD DD` (dongle unpaired) | 0xBBBBBBBB | `DD DD DD DD 02` |
| `EE EE EE EE` + prefix 01 (pipe 0) | 0x77777777 | `EE EE EE EE 01` |

**Proof that a mouse with record `6e 2f 90 c4` matches a dongle with the same record [C]:** the mouse writes BASE1 =
`addr_conv(6e 2f 90 c4)` = 0x76F40923, PREFIX0 = 0x23404380, TXADDRESS = 2, BALEN = 4 (`radio.md` §3). The dongle, with
settings[2..5] = `6e 2f 90 c4` and settings[6] = 1, runs 0x8584–0x85aa: copies settings[2..5] to 0x2000018d,
`set_base_address_1` (0x8f54 → 0xa3b4 → RBIT) → the same 0x76F40923, `update_prefix(2, 0x02)` → the same PREFIX0.
Pipe 2 is enabled (RXADDRESSES=0xFF). The same register values on the same pipe give the same air address, whatever
the actual bit order is. **This dongle's record is `4a 91 3c d7`, so it does NOT match my mouse.**

### 0.3 Mouse → dongle packets (what the dongle accepts)

Received packets are handled by the ESB event handler 0x5a24 (RX_RECEIVED), then by the scheduled handler 0x63cc.
Classification uses `t = (data[1] >> 3) & 3` and `data[1] & 7` **[C]**:

| data[1] | t | Dongle action | Where | Tag |
|---|---|---|---|---|
| **0x68** pairing request | 1 | Only while pairing. Accepted if **data[8] == 0x33** (len ≥ 9). Commits the pairing record (§0.4). Not forwarded to the host | 0x5ac2 | C |
| **0x60** mouse | 0 | Forwarded to the SONiX as report type 0 with **7 bytes data[2..8]** (SPI cmd 0x83, 0x9d4c). 0x200000aa = (data[2] != 0) (button held) | 0x642c | C |
| **0x61** | 0 (low bits 1) | Forwarded as report type 1, data[2..8]. 0x200000ab = any of data[2..8] non-zero | 0x64b4 | C |
| **0x62** | 0 (low bits 2) | Forwarded as report type 2, data[2..8]. 0x200000ac = data[2]\|data[3] non-zero | 0x6538 | C |
| **0x78** keep-alive | 3 | Nothing forwarded. Only refreshes the link timers (§0.6) | 0x6426 | C |
| **0xE0** tunnel response | 0 (bit7) | Reassembled and forwarded to the host as `40 + 64 bytes` (SPI cmd 0x84). See §3 | 0x646a | C |
| t == 2 (0x50–0x57, 0x70–0x77, 0xD0.., 0xF0..) | 2 | Ignored (only the link-alive timer is refreshed) | 0x5ab8 | C |

- Byte data[0] (the mouse's retry counter) is **never read** by the dongle. **[C]**
- Every RX, whatever its type, sets link-alive 0x200001b8=1 and 0x200001ba=1000 (ms). **[C]** (0x5a4e)
- **Duplicate suppression is effectively off [C]:** the SDK check (0x95d4) treats a packet as a retransmit only if
  RXCRC and PID both equal the previous ones. The mouse changes data[0] on each retry, so the CRC differs and a
  retransmit (sent after a lost ACK) is delivered to the host again. It also pops the pending ACK payload as "delivered"
  (0x9644), so an ACK payload whose ACK was lost is dropped. **[C code / H consequence]**
- The RX FIFO holds 8 entries. When it is full the packet is dropped without an ACK (0x95b8). **[C]**

### 0.4 Pairing (dongle side)

**Entry** (all → 0x2000001e=1, 0x20000024 = 30000 ms, radio re-init via state machine 0x8698) **[C]**:
| Trigger | Code | settings[7] marker |
|---|---|---|
| **P0.28 pulled low ≥20 ms, then released** (pad/button; 0x571c, 20-tick debounce 0x2000000e) | 0x574e | **0xCC** |
| SONiX/host command `51 37 00 xx xx xx 01` (echoed back) | 0xa5e6 → 0xa574 | **0xBB** |
| SONiX/host command `B7 47 5A 04 AA 55 5A A5` | 0xa6d2 → 0xa574 | **0xBB** |
| SONiX command 0x81 with `A0 E3 5A A5 01 00 idx` (also forces the pairing channel index to `idx`, 0x200001bf) | 0x7458 → 0xa574 | **0xBB** |

There is no pairing on a timer after power-up. **[C]** (0x2000001e is written only at the four places above and cleared on exit)

**Radio in pairing mode** (0x83fc, pairing branch 0x84c8) **[C]**:
- BASE1 = `31 32 33 34`, prefix(2) = 0x02, pipe 0 = EE…/01, TX power −20 dBm.
- **One fixed channel for the whole 30 s window.** 0x61f0 advances the index 0x20000191 once per pairing entry
  (1→13, then wraps to 1; or uses the override 0x200001bf) and sets `channel = table[idx]`. Table (switch 0x597c):
  idx 0/1→**2**, 2→26, 3→50, 4→74, 5→8, 6→32, 7→56, 8→14, 9→38, 10→62, 11→20, 12→44, 13→68 (the same list the mouse
  hops through every 16 ms). The first pairing after boot uses channel **2**.
- **Own address:** RNG START, `r = RNG.VALUE % 255` (0x9b70, 0x851e), then
  **`addr = { DEVICEID0[0], DEVICEID0[1], DEVICEID0[2]^r, DEVICEID0[3]^r }`**, where DEVICEID0 = FICR 0x10000060, byte 0 =
  LSB (0x842a). It is kept at 0x2000018d.
  - Cross-check with this dump: FICR DEVICEID[0] = `4a 91 ba 51`; the stored record = `4a 91 3c d7`; `ba^3c = 51^d7 = 0x86`
    → one consistent r = 0x86. **[C]**
- **Pairing response** is queued once as the ACK payload on pipe 2 (payload struct 0x200000ae, pipe=2 from RW init), 0x8552–0x857e:

  | byte | 0 | 1 | 2 | 3 | 4..7 |
  |---|---|---|---|---|---|
  | value | **0x01** (type: pairing response) | **0x05** | **0x01** | channel = table[idx] | addr[0..3] |

  Length 8. So the mouse's unresolved bytes 0–2 are **`01 05 01`**. **[C]** (meaning of 05/01: [?], the mouse ignores them)
- **On a valid pairing request** (t==1 && data[8]==0x33, 0x5ac2): pairing flag and timer cleared; 0x2000001d=3; RAM
  settings = `{ch, 0x02, addr[0..3], 0x01, marker}`; 0x200001a8/0x200001b1 = ch. The dongle commits on the **first**
  request it receives. It does not wait to learn whether the ACK reached the mouse. **[C]** (risk: lost ACK → mouse keeps
  hopping, dongle already moved, [H])
- **Save** (main loop 0x5898, when 0x2000001d==3 and ESB is in RX state): settings[7] = marker, flash write (§0.5),
  0x2000001f=2 (status "paired"), radio re-init → paired address, stored channel, +4 dBm. **[C]**
- **Timeout** (0x57f0): after 30 s, 0x2000001f=3 (status "failed") and radio re-init with the old record. **[C]**
- Host status query `37 47 5A`: reply byte 5 = 1 while pairing, 0xAC after success, 0xFA after timeout (0xa67a). **[C]**

**Factory address command** `A4 B0 C0 EA` (0xa752 → 0x6630): generates a new address the same way (DEVICEID0 ^ RNG),
channel = table[0x20000191], settings[6]=1, marker **0xAA**, saves and re-inits. It replies with the 7 record bytes
(`57 12 01` reads them back too). **[C]** The dump's `aa` means this dongle was provisioned this way. The mouse-side
`A4 B0 C1 EA + 7 bytes` (radio.md §7) writes exactly these 7 bytes into the mouse, so factory pairing is
"generate on the dongle, copy to the mouse over USB". **[H]**

### 0.5 Settings page 0x2F000 (format) [C]

Read at boot (0x8414, 2 words → 0x20000010). Written only in 0x5898 (erase page, then program):

| Offset | Size | Content | Dump |
|---|---|---|---|
| 0x00 | 1 | data channel | 02 |
| 0x01 | 1 | constant 0x02 (the pipe/prefix number used) | 02 |
| 0x02 | 4 | base address a0..a3 | 4a 91 3c d7 |
| 0x06 | 1 | paired flag (1 = paired) | 01 |
| 0x07 | 1 | provenance marker: 0xAA factory cmd, 0xBB host-cmd pairing, 0xCC button pairing | aa |
| 0x08 | 4 | N = number of history entries (0xFFFFFFFF → 0; wraps to 0 at 64) | 01 00 00 00 |
| 0x0C | 4 | unused (stays FF) | ff ff ff ff |
| 0x10 | 8·N | history: a copy of each record ever saved, append-only (max 64) | 02 02 4a 91 3c d7 01 aa |

The history can be dumped to the host with command `A4 EA 5A A5` (0xa7a0, streamed from 0x7598). **[C]**
The mouse's 8-byte record uses the same layout for bytes 0..6 (byte 7 = 0xFF). **[C]**

### 0.6 Channel management / link loss [C]

1-ms tick = TIMER1 (0x6a78: PRESCALER 4, 16-bit, CC0=1000, SHORTS COMPARE0_CLEAR, ISR 0x68b8 sets 0x20000020) →
0x6b80 on the main loop. Channel state machine 0x82b0 (state 0x200001a5) plus the counter 0x200001a4:

- Every received packet on the data path (0x63cc end → 0x6118) clears 0x200001a4/0x200001a5. The pairing timer also
  clears them every ms.
- Otherwise 0x200001a4 increments each ms (0x6c1a):
  - at **8 ms** without a packet → state 1: **listen on channel 77**. The first time (0x200001c0==0) the dongle also picks
    the **next data channel** `next = succ(0x200001a8)` (0x612c: 2→26→50→74→8→32→56→14→38→62→20→44→68→2; unknown → 2),
    stores it in 0x200001b1, and queues ACK payload **type 3 `03 01 01 next`** (0x7700; payload struct 0x200000f7, len 4,
    pipe 2). If bit0 of 0x200001c6 (tunnel active) is clear, it flushes the TX FIFO first (0x5820).
  - at **10 ms** → state 3: **listen on channel 5** (same "first time" logic).
  - at **12 ms** → state 5: back to the old data channel 0x200001a8; the counter is set to 4.
  - So without a mouse the dongle cycles: 4 ms old channel, 2 ms ch 77, 2 ms ch 5.
- When a packet arrives while on 77/5 (0x200001a9==1), its ACK carries the queued type-3 payload. The dongle then
  (0x5788) switches to 0x200001b1 (0x200001a8 = new channel) and clears 0x200001c0. The mouse, having taken the type-3
  channel, restores it after its search succeeds (radio.md §8) → both are on the new channel.
- States 2 and 4 (channel 53 and 5) exist in the switch but nothing selects them (dead).
- **No interference detection:** the only trigger for a channel change is 8 ms of silence. The per-channel RX counters
  0x200001ae/0x200001af (incremented in 0x5a6e, saturating at 200) are never read. **[C]**
- The new channel is **not saved to flash** (settings[0] keeps the pairing channel). After a dongle reboot it starts on
  settings[0] again. **[C]**
- **Link-alive:** 0x200001ba = 1000 on every RX, decremented per ms. When the flag 0x200001b8 changes (0x62cc, called
  from 0x7544) the TX FIFO is flushed and the tunnel bit cleared. On a drop to 0 the SONiX gets `.. FF 03 00 00 00`.
  After 1 s without RX the main loop (0x8230) sends all-zero type 0/1/2 reports for any report that was non-zero
  (anti-stuck-button). **[C]**

### 0.7 Dongle → mouse ACK payloads (complete list) [C]

Only three producers call `nrf_esb_write_payload` (0x90e0): 0x857e, 0x772a, 0x77b6. All use pipe 2.

| data[0] | Layout | Len | When | Where |
|---|---|---|---|---|
| **0x01** pairing response | `01 05 01 CH A0 A1 A2 A3` | 8 | queued at pairing radio-init | 0x8552 |
| **0x02** host tunnel | `02 L SEG chunk[≤64]`: L = total command length, SEG = 1,2,… per chunk | chunk+3 (≤ 67) | host command forwarded to the mouse (`50 00 …`, or any unknown command while link-alive) | 0x7738 |
| **0x03** channel change | `03 01 01 CH` | 4 | first entry into link-loss search (§0.6) | 0x7700 |

Empty ACKs (LEN=0) otherwise. The ACK echoes the received S1 (PID and NO_ACK bits): tx[1] = rx[1] at 0x96dc.

---

## 1. Image layout

| Region | Content | Tag |
|---|---|---|
| 0x0000–0x4050 | **Primax bootloader / IRQ forwarder** (SP 0x200029c8, reset 0x251 → SystemInit 0x2e9, `__main` 0x205). Boots the app if flash 0x4010 == `"jump"` and 0x5000 != 0xFFFFFFFF (0x6cc–0x6fc); otherwise it stays in its own SPI-based update loop (SPIM0 0x40004000, NVMC). The IRQ vector stub 0x224 forwards every IRQ to `*(0x5000 + 4·n)` unless 0x20000026==1. No RADIO literals | C |
| 0x4000–0x4050 | "jump" marker page (0x4010) plus the stub's RW init (copy 0x30 B from 0x4014 → 0x20000000, ZI 0x2998 B) | C |
| **0x5000–0xA9E0** | **Application**: vector table 0x5000 (SP **0x20002238**, reset **0x5219**), Keil `__main` scatter-load | C |
| 0x2F000 | settings page (§0.5) | C |

App Region$$Table @0xa958: entry 1 = load 0xa978 → 0x20000000, 0x23C bytes, handler 0x5364 = **zero-run RLE
decompressor** (different from the mouse's LZ one: token `b`: lit=b&15 (0→next byte), run=b>>4 (0→next byte);
copy lit−1 literal bytes, then emit run−1 zero bytes). Entry 2 = ZI 0x1FFC bytes at 0x2000023c. `dscatter.py`
implements it → `ram_app.bin`. **[C]**

Useful RW-init values **[C]**: m_esb_addr 0x2000006c = `E7E7E7E7 C2C2C2C2 | E7 C2 C3 C4 C5 C6 C7 C8 | 08 05 FF 02`;
shorts 0x20000098 = 0x113; payload headers 0x200000ae = `08 02`, 0x200000f7 = `04 02`, 0x20000140 = `08 02`
(len, pipe 2); an unreferenced 17-entry channel copy at 0x20000193 = `2,26,50,74,8,32,56,14,38,62,20,44,68,78,53,29,5`
(dead, as in the mouse).

Side finding (not radio): the nRF↔SONiX link is **SPIM0, not UARTE** (0x9db4: PSEL.SCK=P0.09, MOSI=P0.04,
MISO=P0.05, 2 Mbps, mode 3, CS=P0.06 driven by 0x9e80). There are no UARTE0 (0x40002000) literals. P0.25 (0x7204) is a
request/wake line from the SONiX. **[C]**

## 2. Function map (radio-relevant)

| Addr | Function | Tag |
|---|---|---|
| 0x8d94 | `nrf_esb_init` (config copy → 0x20000b28, MODECNF0, update_radio_parameters 0xa4a0, FIFOs 0x7ee0, TIMER2 0xa104 / PPI 0x9a40 only if PTX, NVIC) | C |
| 0x8c9c / 0x8d20 / 0x8d54 / 0x8e60 | `nrf_esb_disable` / `flush_tx` / `get_clear_interrupts` / "state == PRX-RX(4)" | C |
| 0x8e74 | `nrf_esb_read_rx_payload` (len, pipe, rssi, noack, pid, data@+5) | C |
| 0x8f20 / 0x8f54 / 0x90a8 / 0x8f88 | `set_base_address_0` / `_1` / `update_prefix` / `set_rf_channel` (≤100) | C |
| 0x8fb4 / 0x905c | `nrf_esb_start_rx` / `stop_rx` | C |
| 0x90e0 | `nrf_esb_write_payload` (len 1..68, pipe<8, TX FIFO 8 deep @0x20000d88, per-pipe PID counters 0x20000084) | C |
| 0x959c | `on_radio_disabled_rx` (CRC check, dedupe, ACK payload selection, ACK TX) | C |
| 0x9790 | `on_radio_disabled_rx_ack` (back to RX) | C |
| 0x9c3c | `rx_fifo_push_rfbuf` (RSSISAMPLE → rssi) | C |
| 0xa3b4 / 0x7828 / 0x7ab4 | `update_radio_addresses` / `addr_conv` / `bytewise_bitswap` | C |
| 0xa408 / 0xa450 / 0xa504 / 0xa518 / 0xa548 | bitrate / CRC / TXPOWER / payload format ESB / ESB_DPL | C |
| 0x6244 | RADIO_IRQHandler (vector 0x5044) | C |
| 0x97e0 / 0x98c8 / 0x9f54 | PTX paths (on_radio_disabled_tx / _wait_for_ack / start_tx_transaction); not reached in PRX (mode=1) | C |
| **0x83fc** | **app radio init** (settings, FICR, config, addresses, pairing response, channel, start_rx) | C |
| 0x8698 | radio state machine (0x20000192: 1 = (re)init, 2 = running, 3/4 = suspended) | C |
| **0x5a24** | **ESB event handler** (RX_RECEIVED; pairing commit) | C |
| **0x63cc** | scheduled RX processing (forward to SONiX, tunnel) | C |
| 0x82b0 / 0x5788 / 0x7700 / 0x612c / 0x6b80 | channel search SM / switch to new channel / queue type-3 / successor table / 1 ms tick | C |
| 0x597c / 0x61f0 | pairing channel table / pairing channel advance | C |
| 0x5898 | settings save (flash) + re-init | C |
| 0x571c / 0x57f0 / 0xa574 / 0x6630 | button pairing entry / pairing timeout / host pairing entry / factory address generation | C |
| 0xa5ac | host (SONiX) command dispatcher | C |
| 0x7738 | host → mouse tunnel segmenter (ACK type 2) | C |
| 0x53e4 / 0x539e / 0x5444 | mouse → host tunnel (0xE0) handling | C |
| 0x5d3c | factory RF test mode (entered if **P0.00 is low** 20 ms after boot, 0x9b50); not part of the link | C |

## 3. Tunnel details (type 2 / 0xE0)

**Host → mouse [C]:** the SONiX sends a 0x41-byte frame (SPI 0x2000047d, 0x72c4) → 0xa5ac. `msg[0]` = L (length),
`msg[1..]` = command. For `50 00 …`, or for any command not handled locally while the link is alive, 0x7738 splits it
into ≤64-byte chunks and queues each as `02 L SEG chunk` (SEG from 1). It remembers the first two command bytes
(0x200001c3/4) and sets tunnel bit 0x200001c6.0. If the link is down, the host gets `.. .. 02 EC 05` instead (0xa7ee).
With the usual L = 0x40 this is one 67-byte ACK payload.

**Mouse → host [C/H]:** mouse 0xE0 packet = `rr E0 L SEQ body…` (radio.md). The dongle:
- tunnel bit clear (0x539e): sends `40 body[0..L)` to the host, except bodies starting `FF 03`, which are queued
  (0x53e4/0x5d28, count 0x200001d0 ≤ 16) and emitted later from 0x6482.
- tunnel bit set (0x5444): reassembly into 0x200013f4 (64 B max). The first packet needs SEQ==1 and
  body[0..1] == the stored command bytes. Each later packet needs SEQ == the packet count (0x200001d4). The number of
  chunks is L/64 (+1 for a remainder). When complete: `40` + 64 bytes to the host (cmd 0x12 replies get the dongle
  version from 0x2000003c patched in by 0x56b0), then the tunnel state is cleared. `50 01/02` replies without
  `AC EC 04` make the dongle re-issue a `50 …` request (0x561e). Semantics of these commands: [H]/[?].

## 4. Timing

- PRX turnaround: RX END →(END_DISABLE) DISABLED →(DISABLED_TXEN) TXEN → fast ramp-up (≈40 µs) → READY → START. TIFS
  is not programmed, so the ACK starts about 40–45 µs after the mouse's last CRC bit **[H, datasheet]**. The ACK
  buffer and TXADDRESS are set in the DISABLED ISR (0x959c) during that ramp-up **[C]**. Preamble + address take 48 µs,
  so the mouse sees ADDRESS about 90 µs after its TX END. The mouse opens RX at about 40 µs and gives up 64 µs after
  READY (≈104 µs) → ~14 µs margin **[H]**. A compatible dongle **must** use fast ramp-up (MODECNF0.RU=1), and its ISR
  must load the ACK within ~40 µs.
- The ESB init waits 10 ms (`nrf_delay_us(10000)`, 0x849c) before setting addresses. **[C]**
- Rendezvous: the mouse needs 8 failed transactions per search channel. The dongle visits 77 and 5 for 2 ms each in
  every 8 ms cycle. **[C values / H effectiveness]**

## 5. Recipes

**A. Own mouse firmware → stock dongle** (all [C] unless noted)
1. nrf_esb PTX, DPL, 1 Mbit, CRC16 (0x11021/0xFFFF), LFLEN 8 / MAXLEN 68 / BALEN 4 / big endian, fast ramp-up.
   Pipe 2 = base1 + prefix 0x02; also set pipe 0 = EE EE EE EE / 01 (not needed).
2. Pairing: base1 = `31 32 33 34`. Hop over 2,26,50,74,8,32,56,14,38,62,20,44,68 (the dongle sits on one of them for
   30 s). Send `xx 68 xx xx xx xx xx xx 33` (9 bytes, byte 8 = 0x33). When an ACK payload with `data[0]==0x01` arrives:
   channel = data[3], base1 = data[4..7]. Store `{ch, 02, a0..a3, 01, ff}`.
3. Normal: send 9-byte `rr 60 btn dxL dxH dyL dyH wheel x` (the dongle forwards bytes 2..8 unchanged). Send
   `rr 78 00…` when idle (≤ every few ms; ≥1 packet per 8 ms is needed to avoid the dongle's channel search).
   Setting data[0]=0 on every attempt is fine; the dongle ignores it. Keeping data[0] constant would let the SDK dedupe
   retransmits (same CRC + PID).
4. Handle ACK payloads: `03 01 01 CH` → switch to CH now. `02 L SEG …` → tunnel. Search 77/5 after losing the link.

**B. Compatible dongle (PRX) for the stock mouse:** the §0.1 register set, pipe 2 enabled with the mouse's record, the
channel = record[0]. Always ACK. Queue `03 01 01 CH` only when both sides can move (the mouse applies it
immediately). For pairing: pipe-2 base `31 32 33 34`, one of the 13 channels, TX power low, ACK payload
`01 05 01 CH A0 A1 A2 A3` queued before the mouse's `68` request arrives. **Deduplicate yourself** (e.g. ignore a
packet whose bytes 1..8 equal the previous ones and whose data[0] > 0) to avoid the stock dongle's double-delivery.

## 6. Open questions

1. On-air byte/bit order `a3 a2 a1 a0 02` is derived from the nRF52 address rules, not captured (same caveat as
   radio.md). Needs an SDR / second-nRF sniff.
2. Meaning of pairing response bytes 1–2 (`05 01`), and of type-3 bytes 1–2 (`01 01`). The mouse ignores them.
3. The duplicate delivery on retransmit is inferred from code. Check live whether the SONiX filters it.
4. What P0.28 is physically (a button on the dongle PCB, a test pad, or a SONiX GPIO).
5. Host-side meaning of report types 0/1/2 (SPI 0x83) and of the `FF 03` / `12 01 00` side reports (0x7f84 uses
   data[8] of 0x60/0x62 and data[2] of 0x61 as an 8-bit extra-button bitmap when enabled by `51 36 00 xx 01`, [H]).
6. Full tunnel command set (`50 xx`, `AC EC 04` handshake) and the SONiX USB report mapping - belongs to the USB/tunnel analysis.
7. Exact frame layout of SONiX→nRF SPI commands (0x41-byte read at 0x72c4, the 0x81/0x80/0x20/0x30 opcodes at 0x7204).
