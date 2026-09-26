# HyperX Pulsefire Dart ↔ dongle 2.4 GHz protocol: consolidated spec

Merged from `radio.md` (mouse side, stock fw 1.1.0.8) and `dongle_radio.md` (dongle nRF52810),
cross-checked against the NCS ESB library. **C** = confirmed in code on both sides,
**H** = inferred, not yet captured on air.

## Physical layer (C, identical on both sides)
| Item | Value |
|---|---|
| Stack | Nordic ESB (nRF5 SDK `nrf_esb`), DPL, mouse = PTX, dongle = PRX |
| Bitrate | 1 Mbit/s, fast ramp-up (40 µs) |
| Packet | PCNF0 0x00030008 (LFLEN 8, S1LEN 3), PCNF1 0x01040044 (MAXLEN 68, 5-byte address, big-endian, no whitening) |
| CRC | 16 bit, poly 0x11021, init 0xFFFF |
| Pipe | mouse always transmits on pipe 2; dongle listens on all 8 |
| Addresses | BASE0 `EE EE EE EE` (prefix 0x01, unused); pipe 2 = BASE1 `a0 a1 a2 a3` + prefix 0x02 |
| On-air order | `a3 a2 a1 a0 02` (H, from datasheet rules) |
| TX power | mouse 0 dBm (paired) / −20 dBm (pairing); dongle +4 dBm (paired) / −20 dBm |
| ACK | every packet ACKed; dongle ACK starts ~90 µs after the mouse packet ends |

## Pairing record (8 bytes, same format on both sides)
`CH 02 a0 a1 a2 a3 01 xx`: data channel, constant 2, dongle address, paired flag, source tag.
- Mouse: flash 0xEF000 (+ EEPROM 0x1488 history). My mouse: `02 02 6e 2f 90 c4 01 ff`.
- Dongle: flash 0x2F000 (+ history at 0x2F010). Spare dongle: `02 02 4a 91 3c d7 01 aa`
  (`aa` = written by factory command, `bb` host command, `cc` button).
- Dongle address = FICR DEVICEID0 bytes 0–1, bytes 2–3 XOR a random byte (C).

## Pairing (C)
| Step | Mouse | Dongle |
|---|---|---|
| Trigger | L+R+DPI 5 s (stock) | P0.28 low ≥20 ms then released, or a host command; 30 s window |
| Address | BASE1 `31 32 33 34` ("1234"), prefix 0x02 | same |
| Channel | hops 2,26,50,74,8,32,56,14,38,62,20,44,68 every 16 ms | stays on one of those for the whole window (first pairing after boot: 2) |
| Request | `00 68 02 01 02 03 04 05 33` (9 bytes) | accepts type 0x68 with byte 8 = 0x33 |
| Response | read from the ACK payload | `01 05 01 CH a0 a1 a2 a3` |
| Result | record = `CH 02 a0..a3 01 ff`, saved, radio re-init at 0 dBm | record saved on the first request, before knowing the ACK arrived |

## Mouse → dongle payloads (9 bytes unless noted)
| Byte 1 | Content | Dongle action |
|---|---|---|
| 0x60 | `[2]` buttons, `[3..4]` ΔX int16 LE, `[5..6]` ΔY, `[7]` wheel, `[8]` unused | forwards bytes 2–8 to the USB side |
| 0x61 / 0x62 | 7-byte / 16-bit reports (keyboard/macro, consumer; H) | forwarded |
| 0x68 | pairing request | only while pairing |
| 0x78 | keep-alive, every 4 ms when idle | refreshes link timers; gives it a slot for ACK payloads |
| 0xE0 | tunnel response to a host command, `[2]`=0x40, `[3]` seq, up to 64 bytes | reassembled, sent to host |

Byte 0: stock mouse = retransmit attempt 0/1/2 (changes the CRC per retry, so the dongle
cannot de-duplicate and a retransmit reaches the host twice). The dongle never reads it.

## Dongle → mouse ACK payloads (C)
| Byte 0 | Layout | Meaning |
|---|---|---|
| 1 | `01 05 01 CH a0 a1 a2 a3` | pairing response |
| 2 | `02 L SEG chunk…` (≤64 bytes/chunk) | host (NGENUITY) command tunnel |
| 3 | `03 01 01 CH` | move to channel CH (volatile on the mouse) |

## Channel management / link loss (C)
- Dongle: after 8 ms without packets it queues `03 01 01 next`, then listens 2 ms on ch 77,
  2 ms on ch 5, 4 ms on its old channel, and repeats; it moves once it hears the mouse on 77/5.
- Mouse: after 8 failed transactions it goes to ch 77, toggles 77↔5 every 8 failures, returns
  to its stored channel after 4 toggles.
- Dongle: after 1 s without packets, clears the tunnel and sends zero reports (releases buttons).

## Our firmware vs stock
Same registers and packet formats. Differences: retransmit delay 435 µs (NCS minimum; the
dongle does not care), byte 0 is a per-packet counter so retransmits are de-duplicated,
the command tunnel (NGENUITY) is not implemented, pairing records go to our own settings.

## Open (needs an on-air capture, e.g. an nRF52840 Dongle as sniffer)
On-air address byte order; meaning of `05 01` in the pairing response and `01 01` in the
channel change; what P0.28 is physically on the dongle; the tunnel command set.
