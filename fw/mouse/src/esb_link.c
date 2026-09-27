/*
 * 2.4 GHz link to the stock HyperX dongle: Nordic ESB, PTX, DPL, reproducing the
 * stock mouse radio configuration register-for-register (re/radio.md sections 0-8):
 *   1 Mbit/s, fast ramp-up, CRC16 (0x11021, init 0xFFFF), LFLEN 8 / S1LEN 3 / MAXLEN 68,
 *   BASE0 {EE EE EE EE} prefix 0x01, BASE1 = dongle address, pipe 2 prefix 0x02,
 *   2 retransmits, 0 dBm (−20 dBm while pairing). Retransmit delay is 435 us, the NCS
 *   minimum (stock: 104 us); the dongle is a pure receiver and does not depend on it.
 * NCS's esb.c uses the same addr_conv/bytewise_bit_swap as the nRF5 SDK nrf_esb the
 * stock firmware is built on, so the resulting BASE/PREFIX registers are identical.
 *
 * Differences from stock, on purpose:
 *  - byte 0 of each payload (stock: retransmit attempt 0/1/2, never read by the dongle)
 *    is a per-packet counter 0..2. Retransmits stay byte-identical, so the dongle's ESB
 *    duplicate check drops them (stock mouse retransmits reach the host twice).
 *  - a vendor tunnel reply (NGENUITY) that gets no ACK is resent (up to TUNNEL_RETRIES);
 *    stock sends it once, and a lost reply makes NGENUITY show "Connection lost".
 *  - pairing results go to our settings, the stock page 0xEF000 is left untouched.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/barrier.h>
#include <esb.h>
#include <string.h>

#include "pulsedart.h"
#include "vendor.h"

LOG_MODULE_REGISTER(esbl, LOG_LEVEL_INF);

#define PIPE              2
#define PKT_LEN           9
#define PKT_MOUSE         0x60
#define PKT_KBD           0x61
#define PKT_CONSUMER      0x62
#define PKT_TUNNEL_RESP   0xE0
#define TUNNEL_PKT_LEN    (4 + VENDOR_LEN)
#define PKT_PAIR_REQ      0x68
#define PKT_KEEPALIVE     0x78
#define ACK_PAIR_RESP     1
#define ACK_TUNNEL        2
#define ACK_CHANNEL       3
#define ESB_MAX_CH        100
#define RETX_DELAY_US     435	/* NCS RETRANSMIT_DELAY_MIN */
#define ESB_STALL_MS      500

#define PAIR_HOP_MS       16
#define PAIR_TIMEOUT_MS   65535
#define FAILS_TO_SEARCH   8
#define SEARCH_CH_A       77
#define SEARCH_CH_B       5
#define UNPAIRED_CHANNEL  100

static const uint8_t base0[4] = {0xEE, 0xEE, 0xEE, 0xEE};
static const uint8_t pair_base1[4] = {0x31, 0x32, 0x33, 0x34};	/* "1234" */
static const uint8_t idle_base1[4] = {0xCC, 0xCC, 0xCC, 0xCC};
static const uint8_t prefixes[8] = {0x01, 0xC2, 0x02, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8};
static const uint8_t pair_channels[] = {2, 26, 50, 74, 8, 32, 56, 14, 38, 62, 20, 44, 68};
static const uint8_t pair_req[PKT_LEN] = {0x00, PKT_PAIR_REQ, 0x02, 0x01, 0x02, 0x03, 0x04, 0x05, 0x33};

static bool started;
static bool esb_up;	/* esb_init() succeeded, no esb_disable() since */
static bool parked;	/* ESB disabled while the mouse is idle (releases HFXO) */
static bool pairing;
static uint32_t pair_elapsed_ms;
static uint8_t pair_hop_idx;
static uint8_t data_channel;
static uint8_t fail_count;
static bool searching;
static uint8_t search_fails;
static volatile bool link_up;
static volatile bool pair_done;
static uint8_t pair_rec[8];
static uint8_t tx_seq;
/* NGENUITY commands tunnelled by the dongle (ACK type 2), answered with 0xE0 packets */
static uint8_t tun_req[VENDOR_LEN];
static uint8_t tun_out[VENDOR_LEN];
static volatile bool tun_rx;
static volatile bool tun_tx;	/* set again by the ISR to resend a lost reply */
static atomic_t tun_activity;
static atomic_t link_edge;
static atomic_t tun_seen;	/* any host command arrived (NGENUITY is talking to us) */	/* link came (back) up: the dongle told the host "offline" */
#define TUNNEL_RETRIES 50
static volatile bool tun_inflight;	/* the packet in flight is a tunnel reply */
static volatile uint8_t tun_retries;
static volatile uint8_t in_flight;	/* ESB_LOST_* bit of the report in flight, 0 = none */
static atomic_t lost;			/* ESB_LOST_* bits: main resends that state */
static uint8_t dongle_channel = 0xFF;	/* last channel the dongle moved us to (RAM only) */
static uint32_t busy_ms;

static struct esb_payload tx = {.pipe = PIPE, .length = PKT_LEN};
static struct esb_payload rx;

/*
 * esb_set_rf_channel() only works while ESB is idle, and the event handler runs in
 * interrupt context mid-transaction, so channel changes are requested here and
 * applied from the 1 ms tick (channel_service()).
 */
static volatile uint8_t want_channel;
static uint8_t cur_channel = 0xFF;

static void apply_channel(uint8_t ch)
{
	want_channel = ch;
}

static bool channel_service(void)
{
	if (want_channel == cur_channel) {
		return true;
	}
	if (!esb_is_idle() || esb_set_rf_channel(want_channel)) {
		return false;
	}
	cur_channel = want_channel;
	return true;
}

static void handle_ack_payload(const struct esb_payload *p)
{
	if (pairing) {
		/* dongle 0x8552: 01 05 01 CH A0 A1 A2 A3 (mouse stock 0x546d8 reads [3], [4..7]) */
		if (p->length >= 8 && p->data[0] == ACK_PAIR_RESP && p->data[3] <= ESB_MAX_CH) {
			pair_rec[0] = p->data[3];
			pair_rec[1] = 2;
			memcpy(&pair_rec[2], &p->data[4], 4);
			pair_rec[6] = 1;
			pair_rec[7] = 0xFF;
			compiler_barrier();	/* publish pair_rec before pair_done */
			pair_done = true;
		}
		return;
	}
	if (p->length >= 4 && p->data[0] == ACK_CHANNEL && p->data[3] <= ESB_MAX_CH &&
	    p->data[3] != data_channel) {
		/* volatile (RAM only), like stock and like the dongle itself */
		data_channel = dongle_channel = p->data[3];
		apply_channel(data_channel);
		LOG_INF("dongle moved us to channel %u", data_channel);
	}
	if (p->length > 3 && p->data[0] == ACK_TUNNEL && !tun_rx) {
		/* stock 0x50578: the command is complete after the first segment */
		memset(tun_req, 0, sizeof(tun_req));
		memcpy(tun_req, &p->data[3], MIN(p->length - 3, VENDOR_LEN));
		tun_rx = true;
	}
}

/* ISR (TX_FAILED) or ESB idle/disabled: no concurrent writer */
static void report_unfinished(void)
{
	if (in_flight) {
		atomic_or(&lost, in_flight);
		in_flight = 0;
	}
	/* a tunnel reply lost to TX_FAILED, a failed timeslot, a park or a re-init */
	if (tun_inflight) {
		tun_inflight = false;
		if (tun_retries < TUNNEL_RETRIES) {
			tun_retries++;
			tun_tx = true;
		}
	}
}

static void event_handler(struct esb_evt const *evt)
{
	switch (evt->evt_id) {
	case ESB_EVENT_TX_SUCCESS:
		in_flight = 0;
		tun_inflight = false;
		if (!link_up) {
			atomic_set(&link_edge, 1);
		}
		link_up = true;
		fail_count = 0;
		if (searching) {
			searching = false;
			apply_channel(data_channel);
		}
		break;
	case ESB_EVENT_TX_FAILED:
		esb_flush_tx();
		report_unfinished();
		if (pairing) {
			break;
		}
		/* stock 0x51376: after 8 failures search on 77 <-> 5 */
		if (!searching && ++fail_count >= FAILS_TO_SEARCH) {
			searching = true;
			link_up = false;
			search_fails = 0;
			apply_channel(SEARCH_CH_A);
		} else if (searching && ++search_fails % FAILS_TO_SEARCH == 0) {
			apply_channel((search_fails / FAILS_TO_SEARCH) & 1 ? SEARCH_CH_B : SEARCH_CH_A);
			if (search_fails >= 4 * FAILS_TO_SEARCH) {
				/* back to the stored channel, as stock does after 4 toggles */
				searching = false;
				fail_count = 0;
				apply_channel(data_channel);
			}
		}
		break;
	case ESB_EVENT_RX_RECEIVED:
		while (esb_read_rx_payload(&rx) == 0) {
			handle_ack_payload(&rx);
		}
		break;
	default:
		break;
	}
}

static void wait_esb_idle(void)
{
	for (int i = 0; i < 20 && esb_up && !esb_is_idle(); i++) {
		k_msleep(1);
	}
}

static int radio_config(bool for_pairing)
{
	const struct pd_settings *s = pd_settings_get();
	struct esb_config cfg = ESB_DEFAULT_CONFIG;
	int err;

	wait_esb_idle();
	esb_disable();
	esb_up = false;
	report_unfinished();	/* disabled mid-transaction: no event will come */
	busy_ms = 0;

	cfg.protocol = ESB_PROTOCOL_ESB_DPL;
	cfg.mode = ESB_MODE_PTX;
	cfg.bitrate = ESB_BITRATE_1MBPS;
	cfg.crc = ESB_CRC_16BIT;
	cfg.tx_mode = ESB_TXMODE_AUTO;
	cfg.retransmit_count = 2;
	cfg.retransmit_delay = RETX_DELAY_US;
	cfg.selective_auto_ack = false;
	cfg.use_fast_ramp_up = true;
	cfg.event_handler = event_handler;
	cfg.tx_output_power = (for_pairing || !s->esb_record_valid) ? -20 : 0;

	err = esb_init(&cfg);
	if (err) {
		LOG_ERR("esb_init %d", err);
		return err;
	}
	esb_up = true;
	esb_set_address_length(5);
	esb_set_base_address_0(base0);
	if (for_pairing) {
		esb_set_base_address_1(pair_base1);
		data_channel = pair_channels[0];
	} else if (s->esb_record_valid && s->esb_record[0] <= ESB_MAX_CH) {
		esb_set_base_address_1(&s->esb_record[2]);
		data_channel = dongle_channel <= ESB_MAX_CH ? dongle_channel : s->esb_record[0];
	} else {
		esb_set_base_address_1(idle_base1);
		data_channel = UNPAIRED_CHANNEL;
	}
	esb_set_prefixes(prefixes, ARRAY_SIZE(prefixes));
	esb_enable_pipes(BIT(0) | BIT(PIPE));
	cur_channel = 0xFF;
	apply_channel(data_channel);
	channel_service();

	fail_count = 0;
	searching = false;
	link_up = false;
	return 0;
}

int esbl_start(void)
{
	int err = radio_config(false);

	/* on failure ESB stays uninitialised -> never idle -> the stall watchdog re-inits */
	started = true;
	LOG_INF("ESB link %s, channel %u, %s", err ? "init failed, will retry" : "up", data_channel,
		pd_settings_get()->esb_record_valid ? "paired" : "unpaired");
	return err;
}

bool esbl_ready(void)
{
	return esb_up && !parked && !pairing && pd_settings_get()->esb_record_valid;
}

static int send_len(const uint8_t *pkt, uint8_t len)
{
	int err;

	if (!esb_is_idle() || !channel_service()) {
		return -EBUSY;
	}
	report_unfinished();	/* idle without an event: a failed timeslot (not dispatched) */
	/* idle: no radio ISR can race; drop anything left by TX_FAILED / a failed timeslot */
	esb_flush_tx();

	memcpy(tx.data, pkt, len);
	tx.length = len;
	tun_inflight = pkt[1] == PKT_TUNNEL_RESP && pkt[2] == 0x40;
	tx.data[0] = tx_seq;
	tx_seq = (tx_seq + 1) % 3;
	tx.noack = false;
	in_flight = pkt[1] == PKT_MOUSE	   ? ESB_LOST_MOUSE
		    : pkt[1] == PKT_KBD	   ? ESB_LOST_KBD
		    : pkt[1] == PKT_CONSUMER ? ESB_LOST_CONSUMER
					     : 0;

	/* NCS sets its WAIT_MPSL state after requesting the timeslot; keep other threads
	 * (incl. the MPSL work thread) from running in between
	 */
	k_sched_lock();
	err = esb_write_payload(&tx);
	k_sched_unlock();
	if (err) {
		in_flight = 0;
	}
	return err;
}

static int send_raw(const uint8_t *pkt)
{
	return send_len(pkt, PKT_LEN);
}

int esbl_send(const struct mouse_report *r)
{
	uint8_t pkt[PKT_LEN] = {0x00, PKT_MOUSE, r->buttons & 0x1F,
				(uint8_t)r->dx, (uint8_t)((uint16_t)r->dx >> 8),
				(uint8_t)r->dy, (uint8_t)((uint16_t)r->dy >> 8),
				(uint8_t)r->wheel, 0x00};

	if (!esbl_ready()) {
		return -EAGAIN;
	}
	return send_raw(pkt);
}

/* 0x61: data[2] = modifiers, data[3..8] = keys (stock bug of losing key 2 fixed) */
int esbl_send_kbd(uint8_t mod, const uint8_t keys[6])
{
	uint8_t pkt[PKT_LEN] = {0x00, PKT_KBD, mod};

	if (!esbl_ready()) {
		return -EAGAIN;
	}
	memcpy(&pkt[3], keys, 6);
	return send_raw(pkt);
}

/* 0x62: data[2..3] = 16-bit consumer usage */
int esbl_send_consumer(uint16_t usage)
{
	uint8_t pkt[PKT_LEN] = {0x00, PKT_CONSUMER, usage & 0xFF, usage >> 8};

	if (!esbl_ready()) {
		return -EAGAIN;
	}
	return send_raw(pkt);
}

/*
 * Async status (re/status_ff03.md): stock 0x53a5c(0x20002784, 8) -> 0x506f0 sends
 * 00 E0 08 01 + 9 body bytes (8 status bytes and a zero), length 13. The dongle relays
 * bodies starting FF 03 to the host; NGENUITY reads [4] as "mouse online".
 * Not while a tunnel reply is pending (stock pauses the countdown then).
 */
int esbl_send_status(const uint8_t st[8])
{
	uint8_t pkt[13] = {0x00, PKT_TUNNEL_RESP, 0x08, 0x01};

	if (!esbl_ready()) {
		return -EAGAIN;
	}
	if (tun_rx || tun_tx) {
		return -EBUSY;
	}
	memcpy(&pkt[4], st, 8);
	return send_len(pkt, sizeof(pkt));
}

/* stock sends 0x78 every 4 ms when idle so the dongle can return ACK payloads */
void esbl_keepalive(void)
{
	static const uint8_t pkt[PKT_LEN] = {0x00, PKT_KEEPALIVE};

	if (esbl_ready()) {
		send_raw(pkt);
	}
}

void esbl_start_pairing(void)
{
	if (!started) {
		return;
	}
	pairing = true;
	pair_done = false;
	radio_config(true);
	/* send the first request on channel 2 on the next tick */
	pair_hop_idx = ARRAY_SIZE(pair_channels) - 1;
	pair_elapsed_ms = PAIR_HOP_MS - 1;
	LOG_INF("ESB pairing started");
}

uint8_t esbl_take_lost(void)
{
	return (uint8_t)atomic_clear(&lost);
}

bool esbl_take_tunnel_activity(void)
{
	return atomic_clear(&tun_activity) != 0;
}

bool esbl_take_tunnel_seen(void)
{
	return atomic_clear(&tun_seen) != 0;
}

bool esbl_take_link_edge(void)
{
	return atomic_clear(&link_edge) != 0;
}

bool esbl_pairing(void)
{
	return pairing;
}

void esbl_set_idle(bool idle)
{
	if (!started || pairing) {
		return;
	}
	if (idle && !parked) {
		wait_esb_idle();
		esb_disable();	/* releases HFXO, closes the timeslot session */
		esb_up = false;
		report_unfinished();
		parked = true;
	} else if (!idle && parked) {
		parked = false;
		radio_config(false);
	}
}

void esbl_tick_1ms(void)
{
	/* the stall watchdog must not "recover" a parked link */
	if (!started || parked) {
		return;
	}
	/* recover from a stuck ESB state (lost timeslot, NCS state race) */
	if (esb_is_idle()) {
		busy_ms = 0;
	} else if (++busy_ms >= ESB_STALL_MS) {
		LOG_WRN("ESB stuck for %u ms, re-init", busy_ms);
		busy_ms = 0;
		radio_config(pairing);
		return;
	}
	channel_service();
	if (tun_rx && !tun_tx) {
		uint8_t c0 = tun_req[0], c1 = tun_req[1], c2 = tun_req[2];

		vendor_handle(tun_req, tun_out, false);
		/* stock: D2 and 51 00 00 do not count as user activity */
		if (c0 != 0xD2 && !(c0 == 0x51 && c1 == 0 && c2 == 0)) {
			atomic_set(&tun_activity, 1);
		}
		tun_rx = false;
		tun_retries = 0;
		tun_tx = true;
		atomic_set(&tun_seen, 1);
	}
	if (tun_tx && esbl_ready()) {
		/* stock 0x506f0: 00 E0 40 SEQ + 64 reply bytes; one packet, so SEQ is always 1 */
		uint8_t pkt[TUNNEL_PKT_LEN] = {0x00, PKT_TUNNEL_RESP, 0x40, 0x01};

		memcpy(&pkt[4], tun_out, VENDOR_LEN);
		/* clear first: the ISR may set it again (retry) as soon as the packet is queued */
		tun_tx = false;
		if (send_len(pkt, sizeof(pkt)) == -EBUSY) {
			tun_tx = true;	/* not queued: no event can have touched it */
		}
	}
	if (!pairing) {
		return;
	}
	if (pair_done) {
		struct pd_settings *s = pd_settings_get();

		memcpy(s->esb_record, pair_rec, sizeof(pair_rec));
		s->esb_record_valid = true;
		s->esb_record_dirty = true;
		pd_settings_save();
		pairing = false;
		dongle_channel = 0xFF;	/* the new record starts on its own channel */
		radio_config(false);
		LOG_INF("paired: ch %u addr %02x %02x %02x %02x", pair_rec[0], pair_rec[2],
			pair_rec[3], pair_rec[4], pair_rec[5]);
		return;
	}
	if (++pair_elapsed_ms >= PAIR_TIMEOUT_MS) {
		pairing = false;
		radio_config(false);
		LOG_INF("pairing timed out");
		return;
	}
	/* stock: hop every 16 ms and send the pairing request on the new channel */
	if (pair_elapsed_ms % PAIR_HOP_MS == 0 && esb_is_idle()) {
		pair_hop_idx = (pair_hop_idx + 1) % ARRAY_SIZE(pair_channels);
		esb_flush_tx();
		apply_channel(pair_channels[pair_hop_idx]);
		send_raw(pair_req);
	}
}
