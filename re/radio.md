# 2.4 GHz radio link (mouse side): stock firmware analysis

Source: static analysis of `dump/flash.bin` (main app 0x50000–0x68000), with RW-data initial values
recovered by emulating the Keil `__scatterload` table (`re/tools/rwdata.py` writes `re/tools/ram_init.bin`).
Tags: **[C]** CONFIRMED (seen in code/data), **[H]** HYPOTHESIS (inferred), **[?]** unknown.

Helper scripts are in `re/tools/`: `tdis.py` (range disassembler with literal annotation),
`callers.py` (BL/B.W and function-pointer xrefs), `rwdata.py` (RW-data decompressor), plus
`radio_xref.txt` and `esb_5e600.txt` (raw listings).

---

## 0. How to talk to it (summary)

The link is **Nordic nRF5 SDK `nrf_esb` (Enhanced ShockBurst), PTX role, dynamic payload length (DPL)**,
with a few Primax changes. It is not Gazell: there is no gzll code, and neither the Gazell channel
table nor the Gazell addresses appear anywhere. There is no encryption.

| Item | Value | Tag |
|---|---|---|
| Stack | nRF5 SDK `nrf_esb.c`, protocol `NRF_ESB_PROTOCOL_ESB_DPL`, mode PTX, `tx_mode` AUTO | C |
| RADIO.MODE | **0 = Nrf_1Mbit** (config.bitrate = 0) | C |
| RADIO.MODECNF0 | `|= 1` (RU = Fast ramp-up, 40 µs). Enabled because config byte +0x15 == 40 | C |
| RADIO.TXPOWER | **0x00 (0 dBm)** when paired and not pairing. **0xEC (−20 dBm)** while pairing or unpaired | C |
| RADIO.PCNF0 | **0x00030008**: LFLEN=8, S0LEN=0, S1LEN=3 | C |
| RADIO.PCNF1 | **0x01040044**: MAXLEN=68, STATLEN=0, BALEN=4 (5-byte address), ENDIAN=Big, WHITEEN=0 | C |
| RADIO.CRCCNF / POLY / INIT | **2** (16-bit, address included) / **0x11021** / **0xFFFF** | C |
| DATAWHITEIV, TIFS | not written by the ESB path (whitening off) | C |
| RADIO.SHORTS | common = **0x113** (READY_START, END_DISABLE, ADDRESS_RSSISTART, DISABLED_RSSISTOP); during TX `0x113|0x08` (DISABLED_RXEN) | C |
| TX pipe | **logical pipe 2** (`TXADDRESS=2`, `RXADDRESSES=1<<2` during each TX) | C |
| Pipe-2 address, paired | base1 = settings bytes `6e 2f 90 c4`, prefix = `0x02` → **BASE1=0x76F40923, PREFIX0 byte2 = 0x40** | C |
| Pipe-2 address, pairing | base1 = `31 32 33 34` ("1234"), prefix `0x02` → **BASE1=0x8C4CCC2C** | C |
| Pipe-2 address, unpaired idle | base1 = `CC CC CC CC`, prefix `0x02` → BASE1=0x33333333, channel 100 | C |
| Pipe 0 (unused for TX) | base0 `EE EE EE EE` prefix `0x01` → BASE0=0x77777777 | C |
| PREFIX0 / PREFIX1 | **0x23404380 / 0x13E363A3** (prefixes 01 C2 02 C4 / C5 C6 C7 C8) | C |
| Data channel | from settings page byte 0 (currently **2 → 2402 MHz**). Changed at run time by the dongle's ACK payload (type 3) | C |
| Pairing channels | hops through **2,26,50,74,8,32,56,14,38,62,20,44,68** (one step every 16 ms) | C |
| Link-loss search channels | alternates **77 ↔ 5** (2477/2405 MHz) | C |
| Retransmits | `retransmit_count` = 2 (3 attempts), `retransmit_delay` = 104 µs; ACK wait = 64 µs after RX ramp-up | C |
| Payload (mouse→dongle) | always **pipe 2**. `data[0]` = retransmit counter (0,1,2), `data[1]` = packet type, then body | C |
| Report rate | 1 kHz TIMER1 tick with a divisor table {8,4,2,1} → 125/250/500/1000 Hz | C |

Mouse→dongle packet on air, in the nRF52 `nrf_esb` DPL framing:
`[LEN:8][PID:2|NO_ACK:1][payload LEN bytes][CRC16]`. NO_ACK is sent as 0, so every packet asks for an ACK.

To build a compatible **dongle (PRX)** on nRF52, use SDK `nrf_esb` with the following settings:
- `protocol=ESB_DPL`, `bitrate=NRF_ESB_BITRATE_1MBPS`, `crc=16BIT`.
- `NRF_ESB_MAX_PAYLOAD_LENGTH` > 32 (use 68), so that LFLEN=8 and MAXLEN=68.
- `nrf_esb_set_base_address_1({b7,03,7f,5a})` and `nrf_esb_update_prefix(2, 0x02)`, with pipe 2 enabled. Use `{31,32,33,34}` instead for pairing.
- `nrf_esb_set_rf_channel(2)`.
- Fast ramp-up (MODECNF0.RU=1). The ACK must arrive quickly (see §6).

Queue ACK payloads (type 2 or 3, see §8) on pipe 2.

---

## 1. Where the radio is used

| Region | Radio use | Tag |
|---|---|---|
| 0x00000–0x01000 boot stub | none. The only related literal is 0x4000F518 @0x448 (SystemInit errata-103 CCM.MAXPACKETSIZE) | C |
| 0x27000–0x33000 | none. No RADIO/ECB/TIMER2/PPI literals, and no movw/movt forms. Has 0x4000F000/0x4000F518 (errata code) and RTC1. Strings `main_ptx_with_usb.c`, `app_usbd.c`, `primax_usb.c`, `inner_flash_controller.c`, header `jump…Hans`: a USB updater or bootloader built from the same Primax project | C |
| 0x50000–0x68000 app | all radio code (below) | C |
| movw/movt scan | no `movt #0x4000` peripheral addresses anywhere (all accesses use literal pools) | C |

### Function map (main app)
| Addr | Function (nrf_esb.c name where it matches) | Tag |
|---|---|---|
| 0x5c1c0 | `nrf_esb_init(cfg)`: copies 0x18-byte config to 0x20003cd0, handler to 0x200022b8, sets MODECNF0.RU if cfg[0x15]==40, then `update_radio_parameters`, TIMER2 init (0x618bc), PPI init (0x5ee84), NVIC prio RADIO=cfg[0x11]&7, SWI0=cfg[0x12]&7 | C |
| 0x63780 | `update_radio_parameters` → 0x637e4 txpower, 0x636e8 bitrate, 0x637b0 protocol, 0x63730 crc, then `update_rf_payload_format(cfg.payload_length)` | C |
| 0x63694 | `update_radio_addresses(mask)`: BASE0/BASE1 = `addr_conv` (0x54c60 = REV(bytewise_bitswap)), PREFIX0/1 = `bytewise_bitswap` (0x565a0 = REV(RBIT(x))) | C |
| 0x637f8 / 0x63828 | `update_rf_payload_format_esb` (PCNF0=0x00010100, PCNF1=0x01000000\|(alen-1)<<16\|len<<8\|len) / `_esb_dpl` (PCNF0=0x00030008, PCNF1=0x01000000\|(alen-1)<<16\|0x44) | C |
| 0x61658 | `start_tx_transaction` | C |
| 0x5e6d8 | `on_radio_disabled_tx` | C |
| 0x5e7c0 | `on_radio_disabled_tx_wait_for_ack` (includes the retransmit logic) | C |
| 0x535cc | `RADIO_IRQHandler` (vector 0x50044): READY / END / DISABLED dispatch via 0x200022f0 / 0x200022ec | C |
| 0x539f0 | SWI0_EGU0 IRQ (vector 0x50090): ESB event IRQ → `nrf_esb_get_clear_interrupts` (0x5c180) → app handler | C |
| 0x5c34c / 0x5c380 / 0x5c458 / 0x5c3b4 | `set_base_address_0` / `_1` / `update_prefix(pipe,p)` / `set_rf_channel(ch≤100)` | C |
| 0x5c490 | `nrf_esb_write_payload` (len 1..68, FIFO depth 8, PID = (pid[pipe]+1)%4) | C |
| 0x5c2a0 | `nrf_esb_read_rx_payload` | C |
| 0x5c3e0 / 0x5c14c / 0x5c40c / 0x5c0c8 / 0x5c28c | `start_tx` / `flush_tx` / radio stop (DISABLE, INTENCLR=all) / `nrf_esb_disable` / `is_idle` | C |
| 0x5acbc | **app radio init** (config, addresses, channel) | C |
| 0x51324 | **app ESB event handler** (TX_SUCCESS / TX_FAILED / RX_RECEIVED) | C |
| 0x546d8 | app RX-payload processing (pairing response, tunnel), scheduled from 0x51324 | C |
| 0x5ea44 | **app packet builder / sender** (called every 1 ms tick) | C |
| 0x5358c / 0x50f68 | pairing channel hop / index→channel table | C |
| 0x53554 | link-loss search channel toggle 77↔5 | C |
| 0x51818…0x54968 | factory RF test mode (§11) | C |

---

## 2. ESB configuration (exact values)

Default config literal @0x644ac (24 bytes): `01 00 00 00 | 00 00 00 00 | 00 02 00 00 | 68 00 03 00 | 00 06 07 20 | 00 28 00 00` **[C]**

| Off | Field | Default @0x644ac | Overridden in 0x5acbc | Effective |
|---|---|---|---|---|
| 0x00 | protocol | 1 | =1 | **ESB_DPL** |
| 0x01 | mode | 0 | =0 | **PTX** |
| 0x04 | event_handler | 0 | 0x51325 | 0x51324 |
| 0x08 | bitrate | 0 | – | **Nrf_1Mbit** |
| 0x09 | crc | 2 | – | **16-bit** |
| 0x0A | tx_power | 0 | 0 if (settings.paired==1 && !pairing) else 0xEC | 0 dBm / −20 dBm |
| 0x0C | retransmit_delay (µs) | 0x68 = 104 | – | 104 |
| 0x0E | retransmit_count | 3 | =2 | **2** |
| 0x10 | tx_mode | 0 | – | AUTO |
| 0x11 | radio_irq_priority | 6 | – | 6 |
| 0x12 | event_irq_priority | 7 | – | 7 (SWI0) |
| 0x13 | payload_length (only used in fixed-ESB mode) | 0x20 | – | n/a for DPL |
| 0x14 | selective_auto_ack | 0 | =0 | off → every packet ACKed |
| 0x15 | **ramp-up time µs (Primax addition)** | 0x28 = 40 | – | fast ramp-up |

Derived register writes:
- `update_radio_bitrate` 0x636e8: MODE=cfg.bitrate. ACK timeout `m_wait_for_ack_timeout_us` (0x200022e4) = **64 µs** for MODE 0, 45 µs for MODE 1 (2M), 73 µs for MODE 3 (BLE1M). The stock SDK uses 160/300 µs, so these values were tuned for fast ramp-up. **[C]**
- `update_radio_crc` 0x63730: CRCCNF=2, CRCINIT=0xFFFF, CRCPOLY=0x11021 (the 1-byte case would be INIT 0xFF, POLY 0x107). **[C]**
- `nrf_esb_init` 0x5c20a: `if cfg[0x15]==0x28: MODECNF0 |= 1`. **[C]**
- DPL payload format 0x63828: PCNF0=0x00030008, PCNF1=0x01000000 | ((addr_len−1)<<16) | 0x44. `addr_len` = 5 (RW init of m_esb_addr, never changed) → **0x01040044**. **[C]**
  - LFLEN=8 is needed because `NRF_ESB_MAX_PAYLOAD_LENGTH` is 68 (write_payload rejects len>0x44 @0x5c4b4). This is **not nRF24L01+ compatible** (nRF24 uses a 6-bit length plus a 9-bit PCF). **[C]**

### m_esb_addr (0x200022bc) initial RW values (decompressed) [C]
`base0=E7E7E7E7, base1=C2C2C2C2, prefixes=E7 C2 C3 C4 C5 C6 C7 C8, num_pipes=8, addr_length=5, rx_pipes_enabled=0xFF, rf_channel=2`.
The SDK defaults are then overridden by 0x5acbc (§3).

`m_radio_shorts_common` (0x200022e8) = **0x113**. **[C]**

---

## 3. Addresses, settings page, channel

### Settings page 0xEF000 (8 bytes, read with 2-word copy 0x5760e) [C]
Dump: `02 02 6e 2f 90 c4 01 ff`. The RAM copy is at 0x20002232.

| Byte | Meaning | Value in dump |
|---|---|---|
| 0 | RF channel (data channel from pairing) | 2 (2402 MHz) |
| 1 | constant 2 (written at pairing; prefix/pipe?) | 2 |
| 2–5 | **dongle base address 1** (bytes as passed to `set_base_address_1`) | 6e 2f 90 c4 |
| 6 | paired flag (1 = paired) | 1 |
| 7 | 0xFF | ff |

The page is written at 0x50d10 (erase 0xEF000, write 2 words) after pairing completes (state 0x2000223b==3 and ESB idle). **[C]**
At 0x5a228 the same 8 bytes are also written to an I2C device at 16-bit address 0x1490 through 0x50c40 (TWI helper), guarded by a byte at 0x16a0. So the pairing record is probably mirrored to external storage. **[H]**

### Address derivation [C]
The address is **not** derived from FICR (no FICR DEVICEID/DEVICEADDR literals in the app). It is either fixed or comes from the pairing response stored at 0xEF000.

`0x5acbc`:
- `set_base_address_0({EE,EE,EE,EE})` (literal @0x5ae28), `update_prefix(0, 0x01)`
- if pairing mode (0x2000223d==1): `set_base_address_1({31,32,33,34})` (literal @0x5ae2c), `update_prefix(2, 0x02)`. The channel is not set here because the hop routine sets it.
- elif settings[6]==1: `set_base_address_1(settings[2..5])`, `update_prefix(2,0x02)`, channel = settings[0]
- else: `set_base_address_1({CC,CC,CC,CC})`, `update_prefix(2,0x02)`, settings[0]=100 → channel 100 (2500 MHz)
- 0x20002310 (the run-time channel) = settings[0]; then `set_rf_channel`.

### Register values [C]
| Case | BASE0 | BASE1 | PREFIX0 | PREFIX1 |
|---|---|---|---|---|
| paired (dump) | 0x77777777 | **0x76F40923** | 0x23404380 | 0x13E363A3 |
| pairing | 0x77777777 | **0x8C4CCC2C** | 0x23404380 | 0x13E363A3 |
| unpaired | 0x77777777 | 0x33333333 | 0x23404380 | 0x13E363A3 |

**On-air address [H]:** the nRF52 sends the base address before the prefix, LSbit first, and `addr_conv` bit-reverses every byte. From that, the air sequence (MSbit-first bytes) for the paired pipe-2 address should be **`C4 90 2F 6E 02`**. For an nRF24L01+-style receiver this equals RX_ADDR (LSByte first) = `02 6E 2F 90 C4`. The pairing address would be `34 33 32 31 02` on air. The length field (8 bits) still makes nRF24 hardware unusable (see §2).

---

## 4. Packet format, mouse → dongle (all on pipe 2)

Every payload is built at 0x20002834 (`nrf_esb_payload_t`: len@0, pipe@1, …, data@5) in 0x5ea44 and sent with `nrf_esb_write_payload` (0x5c490).

**Primax change to ESB [C]:** `start_tx_transaction` (0x61720) overwrites **`data[0]` with 0** after copying the payload. `on_radio_disabled_tx_wait_for_ack` (0x5e8a4) **increments `data[0]` before each retransmit**. So `data[0]` is the attempt number (0, 1, 2) and the CRC differs between retransmits. The ESB PID (2 bits) stays the same.

| `data[1]` type | len | Body | Source | Tag |
|---|---|---|---|---|
| **0x60 mouse** | 9 | `data[2]`=buttons (0x20002515), `data[3..4]`=ΔX int16 LE, `data[5..6]`=ΔY int16 LE (PMW3389 burst bytes 2..5 from reg 0x50, buffer 0x200024a4), `data[7]`=wheel int8 (0x200024c8), `data[8]`=not written (stale) | 0x60248, 0x5ebc2 | C (field meaning H) |
| **0x61** (keyboard/macro?) | 9 | `data[2..8]` = 7 bytes from 0x5ff64 (source type 2 of queue 0x58370) | 0x5ebdc | C / meaning H |
| **0x62** (consumer?) | 9 | `data[2..3]` = 16-bit code (queue type 3) | 0x5ebfe | C / meaning H |
| **0x68 pairing request** | 9 | fixed `00 68 02 01 02 03 04 05 33` | 0x5ea80 | C |
| **0x78 idle / keep-alive** | 9 | `00 78 00 00 00 00 00 00 00`, sent every 4 ms (TIMER1 bit2) when there is nothing else to send | 0x5ec8a | C (purpose H: gives the dongle a chance to return ACK payloads) |
| **0xE0 tunnel response** | 4+n (≤68) | `data[0]`=0, `[1]`=0xE0, `[2]`=0x2000430a[2] (=0x40), `[3]`=seq counter (0x20002333++), `[4..]` up to 64 bytes of the response buffer 0x200043d1 | 0x506f0 | C |

Buttons bit mapping: see the sensor/buttons report. The USB descriptor declares 5 buttons, and the same byte is used here. **[H]**

Zero "release" reports: 0x20002262 is set to 1 before a reset or bootloader jump (0x63f0c). While it is nonzero, 0x5eab8 sends all-zero 0x60, then 0x61, then 0x62 packets, and then stops TIMER1. **[C]**

### Report scheduling [C]
- **TIMER1** (init 0x541b8 / restart 0x54220): PRESCALER=4 (1 MHz), 16-bit, CC[0]=1000, SHORTS=COMPARE0_CLEAR, IRQ prio 7 → **1 ms tick**. The ISR at 0x53ff8 sets flags in 0x20002240: bit0 every 1 ms, bit1 every 2 ms, bit2 every 4 ms, bit3 every 8 ms, bit4 every 16 ms (4-bit phase counter 0x2000223f). It also sets 0x2000223e (1 ms software timers, 0x542c0).
- Main loop 0x509bc: bit0 and not pairing → 0x5ea44. bit4 and pairing → hop + 0x5ea44.
- Rate divisor: 0x20002260 counts ticks. A report is built when `counter ≥ table[0x200076c4[0x0e]]`, with the table at 0x66f20 = `{8,4,2,1}` → **125/250/500/1000 Hz**.
- The payload is written only when ESB is idle (0x5ecdc). Startup gate: if P0.09 reads 0, payloads are held until 500 ticks have passed (0x5ece2). **[C]** (why: H)
- Stop/sleep: 0x54270 stops TIMER1 and clears the flags.

---

## 5. Packet format, dongle → mouse (ACK payloads)

These are handled in 0x51324 (RX_RECEIVED) and 0x546d8. The payload is read into 0x2000427e.

| `data[0]` | Meaning | Handling | Tag |
|---|---|---|---|
| **3** | **channel change**: `data[3]` = new RF channel | if ≠ current, 0x20002310 = ch and `set_rf_channel`. Not saved to flash (volatile) | C |
| **2** | **vendor/host tunnel** (host HID vendor report forwarded by the dongle) | 0x50578: copy to 0x200042c7 and set `[1]=0x40`. `data[2]==1` marks the first segment. `data[3..len)` is appended to 0x20004391 (up to 64 bytes). When complete, 0x51c44 processes the command and flag 0x20002323 bit2 is set → the response goes back as 0xE0 packets | C (framing details H) |
| any, while pairing | **pairing response**: `data[3]` = channel, `data[4..7]` = new base address 1 | 0x546d8: settings = {data[3], 2, data[4..7], 1, 0xFF}. Pairing state=3 → flash write → radio re-init | C |

In pairing mode `data[0..2]` of the response are not checked. **[C]** They probably carry a type byte, possibly 3, which would also trigger the channel-change path. **[H]**

---

## 6. ACK / retransmit / timing (TIMER2 + PPI) [C]

- **TIMER2** (0x618bc): PRESCALER=4 (1 MHz), BITMODE=16-bit, SHORTS=0x202 (COMPARE1_CLEAR | COMPARE1_STOP).
- **PPI** (0x5ee84), SDK `NRF_ESB_PPI_*` = channels 10–13:

| CH | EEP | TEP |
|---|---|---|
| 10 | RADIO.EVENTS_READY (0x40001100) | TIMER2.TASKS_START |
| 11 | RADIO.EVENTS_ADDRESS (0x40001104) | TIMER2.TASKS_SHUTDOWN |
| 12 | TIMER2.EVENTS_COMPARE[0] | RADIO.TASKS_DISABLE |
| 13 | TIMER2.EVENTS_COMPARE[1] | RADIO.TASKS_TXEN |

- TX: `SHORTS=0x11B`, `INTENSET=0x11` (READY | DISABLED), TXADDRESS=pipe, RXADDRESSES=1<<pipe, FREQUENCY=rf_channel, PACKETPTR=0x200041d0, TXEN.
- After TX (DISABLED→RXEN short): `SHORTS=0x113`, `CC[0]=64` (ACK wait), `CC[1]=retransmit_delay−ramp = 104−40 = 64`, `CHENSET=ch10–12`, `CHENCLR=ch13`, PACKETPTR=RX buffer 0x20004217.
- ACK received (END and CRCSTATUS OK): TIMER2 SHUTDOWN → TX_SUCCESS. If `rx_buf[0]>0` the ACK payload is pushed to the RX FIFO with its RSSI (RSSISAMPLE 0x40001548 @0x608f0).
- No ACK within 64 µs after the mouse's RX ramp-up → DISABLE. If retries remain: `data[0]++`, TIMER2 START, `CHENSET ch13` → TXEN at CC[1]. After 2 retries → TX_FAILED.
- **Dongle constraint [H]:** the dongle's ACK *address* must arrive within 64 µs of the mouse's RX READY. The mouse reaches RX READY about 40 µs after its TX END (fast ramp-up). Preamble plus address takes 48 µs at 1 Mbps, so the dongle must start transmitting about ≤55 µs after the end of the mouse packet. That requires fast ramp-up on the dongle (nRF52 `nrf_esb` PRX with MODECNF0.RU=1). An nRF24L01+ (130 µs turnaround) cannot meet it.
- Air time at 1 Mbps [H]: 9-byte report ≈ 8+40+8+3+72+16 = 147 µs, empty ACK ≈ 75 µs.

---

## 7. Pairing procedure

1. Entry, 0x51b84: app event 6 (0x599f0(6), probably a button combination) sets pairing flag 0x2000223d=1 and calls LED 0x61314(1).
   - Timeout 0x20002254 = 0xFFFF ms (≈65.5 s), or 500 ms if 0x20002261 is set. It is decremented every 1 ms in 0x542c0; at 0 → 0x50a10 exits pairing.
   - Radio stop (0x5c40c) → re-init (0x5ae4c → state machine 0x5ae6c → 0x5acbc). **[C]**
2. Pairing radio config: TX power −20 dBm, pipe 2 = base "1234"(31 32 33 34) + prefix 0x02. **[C]**
3. Every 16 ms (TIMER1 bit4), 0x509dc → 0x5358c:
   - Index 0x200022f8 goes 1..13 (reset to 0 at 13, then pre-incremented).
   - Channel = 0x50f68(index) = **2,26,50,74,8,32,56,14,38,62,20,44,68**, then `set_rf_channel`.
   - 0x5ea44 then sends the pairing request `00 68 02 01 02 03 04 05 33` (up to 3 attempts). **[C]**
   - A duplicate RW copy of this table plus four extra entries (`2,26,50,74,8,32,56,14,38,62,20,44,68,78,53,29,5`) sits at 0x200022fa. Nothing references it; the code uses the switch table. **[C]**
4. The dongle answers in the ACK payload with channel `data[3]` and address `data[4..7]` (§5).
   - The mouse sets state 3 (0x2000223b), clears the pairing flag and calls 0x5e9dc (LED feedback).
   - The main loop 0x50d10 then erases and rewrites 0xEF000 and re-inits the radio with the new address at 0 dBm. **[C]**
5. The pairing request carries no device-unique ID (bytes 01..05 are constants). The dongle must pick the address. **[C]**

Vendor commands that touch the pairing record, in the NGENUITY-style command handler around 0x53300 (reached over USB or the tunnel; exact opcodes are partly unresolved):
- `A0 xx 5A A5` with byte1 = EA/DA sets or clears an unlock flag (0x20002811).
- Once unlocked, `A4 B0 C1 EA` followed by 7 bytes writes the 7-byte pairing record (0x20002232).
- A read command with byte1=0x13 returns the 7 record bytes (0x5330e / 0x5341a).

All of this is **[C]** as code; the exact opcode semantics are **[H]**.

---

## 8. Channel management and link loss [C]

- The data channel starts at settings[0] and is changed only by the dongle (ACK type 3). The mouse never hops on its own in normal mode.
- TX_FAILED (each one means 3 lost attempts), in 0x51376:
  - Failure counter 0x20002312++. It is **not reset on success**; it is reset only by 0x50a40 (init / resync).
  - When it reaches 8, set the "search" bit (0x2000231b bit0) and switch to channel **77**.
  - In search mode, count 8 failures per channel (bits1..5), then toggle **77↔5** (0x53554) and count toggles in 0x2000231c bits0..3.
  - After 4 toggles, return to 0x20002310, reset the counters, and possibly go to sleep. Sleep uses 0x20002331 > 26, 0x20002314 (9999 ms idle timer) and 0x5ae58 (radio state 3 + TIMER1 stop).
  - The payload stays in the FIFO and `start_tx` is called again (retry), unless both counters are zero.
- TX_SUCCESS while searching: restore channel 0x20002310, reset the counters, set link state 0x20002330=1 and 0x20002314=9999.
- **[H]** The dongle probably listens on 77/5 as rendezvous channels when it loses the mouse, then moves it back with an ACK type 3. Also, because the stored channel is volatile, the dongle has to re-issue type 3 after every mouse reboot if it moved away from settings[0].

---

## 9. Encryption, timers, other peripherals

- **ECB 0x4000E000:** no reference. **CCM/AAR 0x4000F000:** only errata-103 `CCM.MAXPACKETSIZE=0xFB` in SystemInit (0x53ebc; also 0x448 and 0x287xx). **No encryption on the link.** **[C]**
- **RNG:** used only in RF test mode (random test payload / addresses, 0x54908/0x54924). **[C]**
- **TIMER0:** only RF test mode (test-duration timer 0x54968, polled via EVENTS_COMPARE[0] 0x40008140). **TIMER1:** 1 ms application tick. **TIMER2:** ESB. **RTC1:** app_timer (not radio). **[C]**
- **PPI:** channels 10–13 only (ESB). **EGU/SWI:** SWI0_EGU0 IRQ20 is used only as a software-pended ESB event IRQ (NVIC pend/enable). No EGU register access. **[C]**
- **HFXO:** RF test mode starts it explicitly (0x54360: TASKS_HFCLKSTART, wait for HFCLKSTARTED). For normal ESB the start happens elsewhere (clock driver); not traced. **[?]**

---

## 10. Key RAM variables (for debugging live)

| Addr | Meaning |
|---|---|
| 0x20003cd0 | ESB config copy (24 bytes) |
| 0x200022bc | m_esb_addr (base0, base1, prefixes[8], num_pipes, addr_len, rx_pipes, rf_channel@+0x13) |
| 0x200022b0 / 0x200022b1 | ESB initialized / ESB main state (0 idle, 1 TX no-ack, 2 TX-ACK, 3 RX-ACK) |
| 0x200041d0 / 0x20004217 | radio TX / RX buffers ([0]=LEN, [1]=S1, [2..]=payload) |
| 0x20003f30 / 0x200041a4 | TX FIFO / RX FIFO (8 entries, count @+0x28) |
| 0x20002232 | settings copy (0xEF000) |
| 0x20002310 | current data channel |
| 0x2000223d | pairing mode flag, 0x2000223b pairing state (3 = done, save pending) |
| 0x20002312 / 0x2000231b / 0x20002313 | failure counter / search flags / search channel |
| 0x20002330 | link state (1 = connected after success, 3 = lost/sleep) |
| 0x20002834 | outgoing payload being built |
| 0x2000427e | last received ACK payload |
| 0x20002240 / 0x2000223f | TIMER1 tick flags / phase |

---

## 11. Factory RF test mode (not needed for the link, documented for completeness)

Entry: 0x51bcc (from 0x59f3a). If **P0.25 is high at 10 ms and at 100 ms after boot**, an LED is lit (P0.12 direction set) and the firmware enters loop 0x51818, which never returns: HFXO + RNG start, radio config 0x54388, then the command loop. **[C]**

- Base config 0x54388 [C]:
  - Radio: TXPOWER=0, FREQUENCY=100, MODE=0.
  - Addresses: PREFIX0 = bitrev bytes of `C3 C2 C1 CC`, PREFIX1 = bitrev bytes of `C7 C6 C5 C4`, BASE0 = BASE1 = 0x33333333, TXADDRESS=0, RXADDRESSES=1.
  - Packet: PCNF0=0x00010100, PCNF1=0x01020101 (BALEN=2).
  - CRC: CRCCNF=1 (8-bit, POLY 0x107, INIT 0xFF).
  - PACKETPTR=0x2000662c.
- Command reception 0x547b0: RX on this config. A valid packet copies `payload[2 .. 2+payload[1]]` to 0x20002680; the command byte is 0x20002684. It answers with a short TX (PCNF1=0x01020000, SHORTS=0x0B). **[C]** Exact frame layout: **[H]**
- Commands, all with MODE=1 (2 Mbit) and TXPOWER=0 [C]:
  - `C1/C2/C3`: unmodulated carrier (0x546a4) on ch 3/41/79.
  - `C7/C8/C9`: modulated TX (0x544bc) of 254 random bytes (PCNF0=8, PCNF1=0x030400FF, whitening on, CRC off) on ch 3/41/79.
  - `CD/CE/CF`: RX PER test (0x54504) on ch 3/41/79. It counts packets whose first byte is 0x55 and reports the 16-bit count in a 4-byte TX (PCNF1=0x01020303).
  - `DC`: reset.
  - Duration via TIMER0.

---

## 12. Open questions (need a live capture or the dongle firmware)

1. **Air address byte order.** Confirm that the paired address appears on air as `C4 90 2F 6E 02` (and `34 33 32 31 02` for pairing), for example with an SDR or a promiscuous nRF52 sniffer at 2402 MHz, 1 Mbps.
2. **Pairing response format:** what `data[0..2]` contain and how the dongle chooses channel and address (random? derived from its FICR?). What listening channel the dongle uses during pairing.
3. **Dongle link-loss behaviour:** does it listen on 77/5? When and why does it send ACK type 3, and does it do adaptive hopping?
4. **Tunnel (type 2 / 0xE0) framing:** `data[1]`, `data[2]` (segment index or count), and how 64-byte host reports map to segments. How the mouse's 0xE0 responses are reassembled.
5. **Routine ACK payloads:** does the dongle send anything else (battery requests, LED sync, poll-rate changes)? Does the mouse ever report battery over RF? No battery field is seen in 0x60/0x61/0x62/0x78.
6. Whether the dongle uses `data[0]` (retry counter) for duplicate suppression. The CRC changes per retry, so standard ESB CRC+PID dedupe would not catch retransmits.
7. Content of `data[8]` in 0x60 packets (stale byte), and whether the dongle uses it.
8. Meaning of packet types 0x61/0x62 and of the 0x78 keep-alive period in practice (4 ms while idle?).
9. Actual measured retransmit spacing and ACK turnaround. Whether the dongle uses 1 Mbps with fast ramp-up (expected).
10. The poll-rate setting index (0x200076c4+0x0e): default value and the host command that changes it.
11. The I2C mirror of the pairing record (address 0x1490 via 0x50c40): which device it is.
12. Which button combination produces app event 6 (pairing entry), and what 0x20002261 means (500 ms vs 65 s pairing timeout).
