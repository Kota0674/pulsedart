/*
 * NGENUITY vendor protocol, modelled on stock 1.1.0.8 (re/vendor_protocol.md).
 * Host-visible quirks are kept (54 00 00 repeats entry 0, 55/56 address macro 0,
 * 56 offset a<<4, 50 00 01 leaves [3], 50 00 D1 returns 7 bytes, D3 04 ignores bad
 * values); unsafe ones are fixed (D0 02 / D6 bounds, EEPROM page splits).
 * Deferred replies (DE, D1 02, 50 00 D2, A4) are answered immediately: our EEPROM
 * writes run in the background, so the host sees the same echo without the delay.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "pulsedart.h"
#include "stockcfg.h"
#include "actions.h"
#include "leds_fx.h"
#include "vendor.h"

LOG_MODULE_REGISTER(vendor, LOG_LEVEL_INF);

#define OK 0
#define ERR(e) (0x100 | (e))

static const uint8_t product_name[] = "HyperX Pulsefire Dart";
static bool unlocked;		/* A0 EA 5A A5 */
static bool reset_pending;
static bool dfu_pending;

/* custom LED stream state (USB) */
static uint8_t stream_zone;	/* 1 / 2, 0 = none */
static uint8_t stream_off;

/* ---------------- D0 ---------------- */
static int cmd_d0(uint8_t *r)
{
	switch (r[1]) {
	case 0x00:
		if (r[4] >= 4) {
			return ERR(4);
		}
		scfg.cfg[CFG_POLL_IDX] = r[4];
		return OK;
	case 0x01:
		if (r[2] >= 2) {
			return ERR(2);
		}
		if (r[3] >= 2) {
			return ERR(3);
		}
		if (r[4] > 0x30) {
			return ERR(4);
		}
		scfg.cfg[CFG_CUSTOM_CNT(r[2])] = r[4] * 3;
		return OK;
	case 0x02: {
		uint8_t z = r[2], cnt = r[3], st = r[4];

		if (z > 1) {
			return ERR(2);
		}
		if (cnt >= 0x14) {
			return ERR(3);
		}
		if (st >= 0x30) {
			return ERR(4);
		}
		/* stock has no end check here; clip instead of overflowing */
		cnt = MIN(cnt, CUSTOM_LED_ENTRIES - st);
		memcpy(&scfg.custom_led[z][st * 3], &r[5], cnt * 3);
		return OK;
	}
	case 0x54:
		if (r[2] == 0x78) {
			scfg.cfg[CFG_DEBOUNCE_PRESS] = r[4];
			scfg.cfg[CFG_DEBOUNCE_RELEASE] = r[5];
		}
		/* 54 87 (allow deep sleep): accepted; our power policy is independent */
		return OK;
	case 0xAA:
		if (r[2] == 0x55 && r[3] == 0xD0 && r[4] == 0xAA && r[5] == 0x55) {
			/* stock LED test: both zones white, full brightness, static */
			for (int z = 0; z < LED_ZONES; z++) {
				scfg.zone[z][ZONE_BRIGHTNESS] = 100;
				scfg.zone[z][ZONE_EFFECT] = 0;
				scfg.zone[z][ZONE_SUB] = 0;
				memset(&scfg.zone[z][ZONE_RGB1], 0xFF, 6);
			}
			leds_fx_apply_all();
		}
		return OK;
	default:
		return ERR(2);
	}
}

/* ---------------- D1 ---------------- */
static int cmd_d1(uint8_t *r)
{
	switch (r[1]) {
	case 0x00:
		if (r[4] >= 0x1A || r[4] <= 4) {
			return ERR(4);
		}
		scfg.misc[MISC_LOWBAT_PCT] = r[4];
		return OK;
	case 0x01:
		/* stock: EEPROM 0xFE00 := 1 (gauge re-program by the stock FW), reset.
		 * We never write the EEPROM outside DE: reset only.
		 */
		reset_pending = true;
		return OK;
	case 0x02:
		return OK;	/* stock: EEPROM 0xFE01 := 0; no-op here */
	case 0x94:
		if (r[2] == 0x78) {
			scfg.misc[MISC_LOWBAT_PCT] = r[4];
		}
		return OK;
	default:
		return ERR(1);
	}
}

/* ---------------- D2 ---------------- */
static int d2_zone(uint8_t z, const uint8_t *r)
{
	uint8_t *b = scfg.zone[z];

	b[ZONE_BRIGHTNESS] = r[10];	/* stock stores it before the mode check */
	if ((r[2] >> 4) >= 5 || (r[2] & 0xF) >= 4) {
		return ERR(2);
	}
	b[ZONE_EFFECT] = r[2] >> 4;
	b[ZONE_SUB] = r[2] & 0xF;
	b[ZONE_SPEED] = r[11];
	memcpy(&b[ZONE_RGB1], &r[4], 6);
	return OK;
}

static int cmd_d2(uint8_t *r)
{
	uint8_t z = r[1] >> 4;
	int err = OK;

	if (z >= 3) {
		return ERR(1);
	}
	if (r[10] >= 0x65) {
		return ERR(1);
	}
	if (z < 2) {
		err = d2_zone(z, r);
		if (!err) {
			leds_fx_apply_zone(z);
		}
		return err;
	}
	for (int i = 0; i < LED_ZONES; i++) {
		int e = d2_zone(i, r);

		if (e) {
			err = e;
		}
	}
	leds_fx_apply_all();
	return err;
}

/* ---------------- D3 ---------------- */
static int cmd_d3(uint8_t *r)
{
	uint8_t s = r[2];
	uint16_t v = r[4] | (r[5] << 8);

	switch (r[1]) {
	case 0x00:
		if (r[4] >= DPI_STAGES || !(scfg.dpi[DPI_MASK] & BIT(r[4]))) {
			return ERR(4);
		}
		scfg.dpi[DPI_CUR] = r[4];
		dpi_apply();
		return OK;
	case 0x01: {
		uint8_t m = r[4], n = 0, first = 0;

		if (r[2]) {
			return ERR(2);
		}
		if (m > 0x1F) {
			return ERR(4);
		}
		scfg.dpi[DPI_MASK] = m;
		for (int i = DPI_STAGES - 1; i >= 0; i--) {
			if (m & BIT(i)) {
				n++;
				first = i;
			}
		}
		scfg.dpi[DPI_COUNT] = n;
		scfg.dpi[DPI_CUR] = first;
		dpi_apply();
		return OK;
	}
	case 0x02:
		if (s >= DPI_STAGES || !(scfg.dpi[DPI_MASK] & BIT(s))) {
			return ERR(2);
		}
		if (v < 2 || v > 320) {
			return ERR(4);
		}
		sc_put16(&scfg.dpi[DPI_VAL(s)], v);
		if (s == scfg.dpi[DPI_CUR]) {
			dpi_apply();
		}
		return OK;
	case 0x03:
		if (s >= DPI_STAGES || !(scfg.dpi[DPI_MASK] & BIT(s))) {
			return ERR(2);
		}
		memcpy(&scfg.dpi[DPI_RGB(s)], &r[4], 3);
		return OK;
	case 0x04:
		if (r[2]) {
			return ERR(2);
		}
		if (v >= 2 && v <= 320) {	/* stock ignores bad values silently */
			sc_put16(&scfg.dpi[DPI_SNIPER], v);
		}
		return OK;
	default:
		return ERR(1);
	}
}

/* ---------------- D4 / D5 / D6 ---------------- */
static uint8_t aux_of(uint8_t t, uint8_t c)
{
	if (t == 1) {
		return c >= 1 && c <= 8 ? BIT(c - 1) : 0;
	}
	if (t == 3) {
		return c < 8 ? BIT(c) : 0;
	}
	return c;
}

static int cmd_d4(uint8_t *r)
{
	uint8_t b = r[1], t = r[2], c = r[4];

	if (b >= 6) {
		return ERR(1);
	}
	if (t >= 8) {
		return ERR(2);
	}
	scfg.btn[b * 3] = t;
	scfg.btn[b * 3 + 1] = c;
	scfg.btn[b * 3 + 2] = aux_of(t, c);
	return OK;
}

static int cmd_d5(uint8_t *r)
{
	uint8_t m = r[1];
	uint16_t len = r[4] | (r[5] << 8);

	if (r[2]) {
		return ERR(2);
	}
	if ((m >> 4) || (m & 0xF) >= MACROS) {
		return ERR(1);
	}
	if (len > 0x200 || r[6] > 3) {
		return ERR(4);
	}
	uint8_t *h = scfg.macro_hdr[m & 0xF];

	sc_put16(&h[0], len);
	h[6] = r[6];
	sc_put16(&h[7], r[6] == 1 ? r[7] : 0);
	return OK;
}

static int cmd_d6(uint8_t *r)
{
	uint8_t m = r[1];
	uint16_t o = (r[2] << 2) | (r[3] >> 6);
	uint8_t n = r[3] & 0x3F;

	if ((m >> 4) || (m & 0xF) >= MACROS) {
		return ERR(2);
	}
	if (o >= 0x200 || n > 6) {
		return ERR(1);
	}
	uint8_t *h = scfg.macro_hdr[m & 0xF];
	uint8_t *d = scfg.macro_data[m & 0xF];
	uint16_t n5 = 0, n10 = 0, pos = 0;

	for (uint16_t i = 0; i < o && pos < MACRO_DATA_LEN; i++) {
		uint8_t sz = d[pos] == 0x1A ? 10 : 5;

		pos += sz;
		sz == 10 ? n10++ : n5++;
	}
	const uint8_t *src = &r[4];

	for (uint8_t i = 0; i < n; i++) {
		uint8_t sz = src[0] == 0x1A ? 10 : 5;

		if (pos + sz > MACRO_DATA_CAP(m & 0xF) || src + sz > r + VENDOR_LEN) {
			break;	/* stock has no bound check: stop instead of overflowing */
		}
		memcpy(&d[pos], src, sz);
		pos += sz;
		src += sz;
		sz == 10 ? n10++ : n5++;
	}
	sc_put16(&h[2], n5);
	sc_put16(&h[4], n10);
	return OK;
}

/* ---------------- DE / DF ---------------- */
static int cmd_de(uint8_t *r)
{
	uint8_t k = r[1];

	if (k != 0xFF && k >= 6) {
		return ERR(1);
	}
	stockcfg_save(k == 0xFF ? BLK_ALL : k);
	return OK;
}

static int cmd_df(uint8_t *r)
{
	uint8_t k = r[2];

	if (r[1] != 0xAA) {
		return ERR(1);
	}
	if (k != 0xFF && k >= 6) {
		return ERR(2);
	}
	if (k == BLK_MISC || k == 0xFF) {
		scfg.misc[MISC_LOWBAT_PCT] = 15;	/* stock restores only [2] */
	}
	if (k == BLK_MACROS || k == 0xFF) {
		for (int i = 0; i < MACROS; i++) {
			memset(scfg.macro_hdr[i], 0, 6);	/* len and counters only */
		}
	}
	for (int b = BLK_CFG; b <= BLK_BUTTONS; b++) {
		if ((k == b || k == 0xFF) && b != BLK_MISC) {
			stockcfg_defaults(b);
		}
	}
	if (k == BLK_LED || k == 0xFF) {
		leds_fx_apply_all();
	}
	if (k == BLK_MACROS || k == BLK_BUTTONS || k == 0xFF) {
		actions_reset();
	}
	if (k == BLK_DPI || k == 0xFF) {
		dpi_apply();
	}
	return OK;
}

/* ---------------- reads ---------------- */
/* radio (dongle) stream, stock 0x5296a / 0x52bdc: continuation "AC EC 01", end "AC EC 04" */
static struct {
	uint8_t hdr[3];
	uint8_t total, off, rem;
	const uint8_t *ptr;
} rs;

static void stream_radio(uint8_t *r)
{
	if (r[4] == 0xAC && r[5] == 0xEC && r[6] == 0x01) {
		memcpy(r, rs.hdr, 3);
		if (!rs.ptr || rs.rem == 0) {
			r[3] = 0;
			r[4] = 0xAC;
			r[5] = 0xEC;
			r[6] = 0x04;	/* end marker */
		} else if (rs.rem < 60) {
			memcpy(&r[4], rs.ptr + rs.off, rs.rem);
			r[3] = rs.rem / 3;
			rs.rem = 0;
		} else {
			memcpy(&r[4], rs.ptr + rs.off, 60);
			r[3] = 20;
			rs.off += 60;
			rs.rem = rs.total - rs.off;
		}
		return;
	}
	uint8_t z = r[1];
	uint8_t t = MIN(scfg.cfg[CFG_CUSTOM_CNT(z - 1)], CUSTOM_LED_LEN);

	rs.hdr[0] = 0x50;
	rs.hdr[1] = z;
	rs.hdr[2] = 0;
	rs.ptr = scfg.custom_led[z - 1];
	r[2] = 0;
	if (t < 60) {
		r[3] = t / 3;
		memcpy(&r[4], rs.ptr, t);
		rs.rem = 0;
	} else {
		r[3] = 20;
		memcpy(&r[4], rs.ptr, 60);
		rs.total = t;
		rs.off = 60;
		rs.rem = t - 60;
	}
}

static void stream_first(uint8_t *r, uint8_t z)
{
	uint8_t total = MIN(scfg.cfg[CFG_CUSTOM_CNT(z - 1)], CUSTOM_LED_LEN);
	const uint8_t *a = scfg.custom_led[z - 1];

	r[0] = 0x50;
	r[1] = z;
	r[2] = 0;
	stream_zone = z;
	stream_off = 0;
	if (total == 0) {
		stream_zone = 0;
		return;
	}
	if (total < 60) {
		r[3] = total / 3;
		memcpy(&r[4], a, total);
		stream_zone = 0;
	} else {
		r[3] = 20;
		memcpy(&r[4], a, 60);
		stream_off = 60;
	}
}

bool vendor_stream_next(uint8_t *p)
{
	if (!stream_zone) {
		return false;
	}
	uint8_t z = stream_zone;
	uint8_t total = MIN(scfg.cfg[CFG_CUSTOM_CNT(z - 1)], CUSTOM_LED_LEN);
	const uint8_t *a = scfg.custom_led[z - 1];

	memset(p, 0, VENDOR_LEN);
	p[0] = 0x50;
	p[1] = z;
	if (total > stream_off) {
		uint8_t rem = total - stream_off;

		p[2] = z == 1 ? stream_off / 3 : stream_off;	/* stock inconsistency, kept */
		if (rem < 60) {
			memcpy(&p[4], a + stream_off, rem);
			p[3] = rem / 3;
			stream_zone = 0;
			stream_off = 0;
		} else {
			memcpy(&p[4], a + stream_off, 60);
			p[3] = 20;
			stream_off += 60;
		}
	} else {
		stream_zone = 0;	/* terminator: 50 z 00 00 ... */
		stream_off = 0;
	}
	return true;
}

static int cmd_50(uint8_t *r, bool usb)
{
	switch (r[1]) {
	case 0x00:
		switch (r[2]) {
		case 0x00:
			r[3] = 0x1D;
			r[4] = 0xE2; r[5] = 0x16;	/* PID */
			r[6] = 0x51; r[7] = 0x09;	/* VID */
			r[8] = 0x08; r[9] = 0x00; r[10] = 0x01; r[11] = 0x01;
			memcpy(&r[12], product_name, sizeof(product_name));
			return OK;
		case 0x01: {
			static const uint8_t v[8] = {0x02, 0x0E, 0x01, 0x05, 0x08, 0x00, 0x01, 0x01};

			memcpy(&r[4], v, 8);
			return OK;
		}
		case 0xD0:
			r[3] = 1;
			r[4] = stockcfg_pair_count;
			return OK;
		case 0xD1: {
			uint8_t rec[8] = {0};

			if (r[3] < 8) {
				memcpy(rec, stockcfg_pair_hist[r[3]], 8);
			} else {
				eeprom_read(0x1490 + 8 * r[3], rec, 8);
			}
			r[3] = 8;
			memcpy(&r[4], rec, 7);	/* stock returns 7 of 8 */
			return OK;
		}
		case 0xD2:
			return OK;	/* stock: save the pairing record to EEPROM history; ours lives in settings */
		default:
			return ERR(2);
		}
	case 0x01:
	case 0x02:
		if (usb) {
			stream_first(r, r[1]);
		} else {
			stream_radio(r);
		}
		return OK;
	case 0x03:
		r[3] = 0x31;
		r[4] = scfg.zone[0][ZONE_EFFECT];
		r[5] = scfg.zone[1][ZONE_EFFECT];
		r[6] = scfg.dpi[DPI_COUNT];
		memcpy(&r[7], &scfg.dpi[DPI_SNIPER], 2);
		memcpy(&r[9], &scfg.dpi[DPI_VAL(0)], 10);
		memcpy(&r[19], &scfg.dpi[DPI_RGB(0)], 15);
		memcpy(&r[34], scfg.btn, 18);
		r[52] = scfg.cfg[CFG_POLL_IDX];
		return OK;
	default:
		return ERR(1);
	}
}

static int cmd_51(uint8_t *r)
{
	if (r[1] == 0x00) {
		if (r[2]) {
			return ERR(2);
		}
		const struct power_status *p = power_get();

		scfg.misc[MISC_SOC] = p->soc <= 100 ? p->soc : 0;
		scfg.misc[MISC_POWER_STATE] = p->ext_power ? (p->charging ? 1 : 3) : 0;
		sc_put16(&scfg.misc[MISC_MV], p->mv);
		r[3] = 3;
		memcpy(&r[4], &scfg.misc[0], 3);
		memcpy(&r[7], &scfg.misc[MISC_MV], 2);
		scfg.misc[MISC_GAUGE_STATUS] = power_gauge_status();
		r[9] = scfg.misc[MISC_GAUGE_STATUS];
		r[10] = 0x40;	/* gauge wait counter: stock value after init */
		r[11] = r[12] = r[13] = 0;
		return OK;
	}
	if (r[1] == 0x01) {
		if (r[2] >= 2) {
			return ERR(2);
		}
		uint16_t v = 0;

		if (r[2] == 0) {
			power_gauge_read16(r[4], &v);
		} else {
			power_gauge_control(r[4], &v);	/* stock: 8-bit sub-command */
		}
		r[3] = 0;
		r[5] = v;
		r[6] = v >> 8;
		r[7] = r[8] = 0;
		return OK;
	}
	return ERR(1);
}

static int cmd_52(uint8_t *r)
{
	if (r[1]) {
		return ERR(1);
	}
	if (r[2]) {
		return ERR(2);
	}
	r[3] = 0x11;
	for (int z = 0; z < LED_ZONES; z++) {
		const uint8_t *b = scfg.zone[z];
		uint8_t *o = &r[4 + 10 * z];

		o[0] = b[ZONE_EFFECT];
		o[1] = b[ZONE_SUB];
		o[2] = b[ZONE_SPEED];
		o[3] = b[ZONE_BRIGHTNESS];
		memcpy(&o[4], &b[ZONE_RGB1], 6);
	}
	return OK;
}

static int cmd_53(uint8_t *r)
{
	if (r[1] == 0x00) {
		if (r[2]) {
			return ERR(2);
		}
		r[3] = 0x21;
		r[4] = scfg.dpi[DPI_CUR];
		r[5] = scfg.dpi[DPI_MASK];
		memcpy(&r[6], &scfg.dpi[DPI_MIN], 4);
		memcpy(&r[10], &scfg.dpi[DPI_SNIPER], 2);
		memcpy(&r[12], &scfg.dpi[DPI_VAL(0)], 10);
		memcpy(&r[22], &scfg.dpi[DPI_RGB(0)], 15);
		return OK;
	}
	if (r[1] == 0x94) {
		if (r[2] == 0x87) {
			r[5] = sensor_read_reg(r[4]);
		}
		return OK;
	}
	return ERR(1);
}

static int cmd_54(uint8_t *r)
{
	if (r[1]) {
		return ERR(1);
	}
	if (r[2]) {
		return ERR(2);
	}
	r[3] = 0x0C;
	for (int i = 0; i < 6; i++) {
		memcpy(&r[4 + 3 * i], scfg.btn, 3);	/* stock bug: entry 0 repeated, kept */
	}
	return OK;
}

static int cmd_55(uint8_t *r)
{
	if (r[1] >> 4) {
		return ERR(1);
	}
	const uint8_t *h = scfg.macro_hdr[0];	/* stock: index = m & 0xF0 = 0 */

	memcpy(&r[4], &h[0], 2);
	r[6] = h[6];
	memcpy(&r[7], &h[7], 2);
	return OK;
}

static int cmd_56(uint8_t *r)
{
	uint8_t n = r[3] & 0x3F;
	uint16_t o = (r[2] << 4) | (r[3] >> 6);	/* stock: a<<4 here, a<<2 in D6 */

	if (r[1] >> 4) {
		return ERR(1);
	}
	if (n > 6 || o >= 0x200) {
		return ERR(2);
	}
	const uint8_t *d = scfg.macro_data[0];
	uint16_t pos = 0, ev = 0, w = 4;

	while (pos < MACRO_DATA_LEN && ev < o) {
		pos += d[pos] == 0x1A ? 10 : 5;
		ev++;
	}
	for (uint8_t i = 0; i < n && pos < MACRO_DATA_LEN; i++) {
		uint8_t sz = d[pos] == 0x1A ? 10 : 5;

		if (w + sz > VENDOR_LEN || pos + sz > MACRO_DATA_LEN) {
			break;
		}
		memcpy(&r[w], &d[pos], sz);
		w += sz;
		pos += sz;
	}
	return OK;
}

static int cmd_57(uint8_t *r)
{
	const struct pd_settings *s = pd_settings_get();

	if (r[1] != 0x13 || r[2] != 0x01 || r[3] != 0x00) {
		return ERR(0);
	}
	r[3] = 8;
	memcpy(&r[4], s->esb_record, 7);
	return OK;
}

/* ---------------- bootloader / ID (A0..A4) ---------------- */
static const uint8_t dfu_key[10] = {0x00, 0x4B, 0xB4, 0x94, 0x10, 0x98, 0x27, 0x24, 0x10, 0x00};

static int cmd_ax(uint8_t *r, bool usb)
{
	switch (r[0]) {
	case 0xA0: {
		bool good = r[2] == 0x5A && r[3] == 0xA5 && (r[1] == 0xEA || r[1] == 0xDA);

		if (good) {
			unlocked = r[1] == 0xEA;
		}
		r[2] = 0x02;
		r[3] = 0x00;
		r[4] = 0xEC;
		r[5] = good ? 0xAC : 0xFE;
		return OK;
	}
	case 0xA1: {
		/* DFU needs the cable: the bootloader only talks USB */
		bool good = usb && unlocked && !memcmp(&r[1], dfu_key, sizeof(dfu_key)) &&
			    r[11] == 0x01;
#if CONFIG_BOOTLOADER_MCUBOOT
		/*
		 * No HyperX bootloader behind MCUboot, and 0x27000 lies inside slot 0: an
		 * NGENUITY "firmware update" must be refused, never write the stock header.
		 */
		good = false;
#endif

		r[2] = 0x02;
		r[3] = 0x00;
		r[4] = 0xEC;
		r[5] = good ? 0xAC : 0xFE;
		if (good) {
			dfu_pending = true;
		}
		return OK;
	}
	case 0xA2:
		if (!unlocked) {
			return ERR(0);
		}
		if (r[1] == 0x10) {
			static const uint8_t tag[4] = {0x65, 0x09, 0x00, 0x02};	/* stock flash 0x50200 */

			r[2] = 0x04;
			r[3] = 0x00;
			memcpy(&r[4], tag, 4);
			return OK;
		}
		if (r[1] == 0x13) {
			if (r[2] != 0x01 || r[3] != 0x00) {
				return ERR(2);
			}
			r[2] = 0x08;
			r[3] = 0x00;
			memcpy(&r[8], pd_settings_get()->esb_record, 7);
			return OK;
		}
		return ERR(1);
	case 0xA4:
		if (unlocked && usb && r[1] == 0xB0 && r[2] == 0xC1 && r[3] == 0xEA) {
			struct pd_settings *s = pd_settings_get();

			memcpy(s->esb_record, &r[4], 7);
			s->esb_record[7] = 0xFF;
			s->esb_record_valid = s->esb_record[6] == 1;
			s->esb_record_dirty = true;
			pd_settings_save();
			return OK;
		}
		return ERR(0);
	default:
		return ERR(0);
	}
}

bool vendor_handle(uint8_t *req, uint8_t *out, bool usb)
{
	int res;
	uint8_t c = req[0];

	if (c >= 0xD0 && c <= 0xDF) {
		switch (c) {
		case 0xD0: res = cmd_d0(req); break;
		case 0xD1: res = cmd_d1(req); break;
		case 0xD2: res = cmd_d2(req); break;
		case 0xD3: res = cmd_d3(req); break;
		case 0xD4: res = cmd_d4(req); break;
		case 0xD5: res = cmd_d5(req); break;
		case 0xD6: res = cmd_d6(req); break;
		case 0xDE: res = cmd_de(req); break;
		case 0xDF: res = cmd_df(req); break;
		default: res = ERR(1); break;
		}
	} else if (c >= 0xF0) {
		res = ERR(1);
	} else {
		switch (c) {
		case 0x50: res = cmd_50(req, usb); break;
		case 0x51: res = cmd_51(req); break;
		case 0x52: res = cmd_52(req); break;
		case 0x53: res = cmd_53(req); break;
		case 0x54: res = cmd_54(req); break;
		case 0x55: res = cmd_55(req); break;
		case 0x56: res = cmd_56(req); break;
		case 0x57: res = cmd_57(req); break;
		case 0xA0: case 0xA1: case 0xA2: case 0xA4:
			res = cmd_ax(req, usb);
			break;
		default: res = ERR(0); break;
		}
	}

	memset(out, 0, VENDOR_LEN);
	if (res == OK) {
		memcpy(out, req, VENDOR_LEN);	/* reply = request modified in place */
	} else {
		out[0] = req[0];
		out[1] = req[1];
		out[2] = req[2];
		out[3] = 0x02;
		out[4] = 0xEC;
		out[5] = res & 0xFF;
	}
	return true;
}

/* stock 0x56cd8: clear "jump" in the boot header page 0x27000 and reset into the
 * USB bootloader (VID 0951 PID 16F8), which then waits for a firmware image
 */
#if !CONFIG_BOOTLOADER_MCUBOOT
static void enter_dfu(void)
{
	const struct device *flash = DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));
	uint8_t hdr[28];

	if (flash_read(flash, 0x27000, hdr, sizeof(hdr))) {
		return;
	}
	memset(&hdr[0x10], 0xFF, 4);
	if (flash_erase(flash, 0x27000, 0x1000) || flash_write(flash, 0x27000, hdr, sizeof(hdr))) {
		LOG_ERR("DFU header write failed");
		return;
	}
	k_msleep(300);
	sys_reboot(SYS_REBOOT_COLD);
}
#endif

void vendor_poll(void)
{
	if (reset_pending) {
		stockcfg_flush();
		k_msleep(50);	/* let the echo go out */
		sys_reboot(SYS_REBOOT_COLD);
	}
#if !CONFIG_BOOTLOADER_MCUBOOT
	if (dfu_pending) {
		dfu_pending = false;
		stockcfg_flush();
		k_msleep(50);
		enter_dfu();
	}
#endif
}
