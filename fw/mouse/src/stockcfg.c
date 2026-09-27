/*
 * Stock-compatible settings in the external EEPROM (I2C 0x50, 16-bit addresses,
 * 128-byte pages). Defaults are the stock RW-init values (re/input_usb_led.md 5).
 * Writes run on a work queue so the 1 kHz loop never waits for the EEPROM.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "stockcfg.h"

LOG_MODULE_REGISTER(stockcfg, LOG_LEVEL_INF);

#define EEPROM_ADDR      0x50
#define EEPROM_PAGE      128
#define EEPROM_WRITE_MS  30	/* stock waits 30 ms after each write */

#define EE_FW_VERSION    0x0001
#define EE_LAYOUT        0x0010
#define EE_MISC          0x0060
#define EE_DPI           0x0090
#define EE_LED           0x0110
#define EE_BUTTONS       0x0190
#define EE_MACRO_HDR(i)  (0x0210 + 16 * (i))
#define EE_CFG           0x0290
#define EE_CUSTOM_A(z)   (0x0490 + 0x200 * (z))	/* 0x70 bytes */
#define EE_CUSTOM_B(z)   (0x0500 + 0x200 * (z))	/* 0x20 bytes */
#define EE_MACRO_DATA(i) (0x0890 + 0x200 * (i))

static const uint8_t fw_version[4] = {0x08, 0x00, 0x01, 0x01};	/* 1.1.0.8, as stock */
static const uint8_t layout_version[4] = {0x07, 0x00, 0x07, 0x04};	/* 0x4707 */

static const uint8_t def_cfg[CFG_LEN] = {
	0x01, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x03, 0x03, 0x03, 0x12, 0x10, 0x27, 0x00, 0x00, 0x60, 0xea, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t def_misc[MISC_LEN] = {0x00, 0x00, 0x0f, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00};
static const uint8_t def_dpi[DPI_LEN] = {
	0x00, 0x01, 0x03, 0x02, 0x00, 0x40, 0x01, 0x07, 0x10, 0x00,
	0x10, 0x00, 0x20, 0x00, 0x40, 0x00, 0x80, 0x00, 0x40, 0x01,
	0x00, 0x00, 0x00,
	0x00, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0x00, 0xff, 0xc0, 0xcb,
};
static const uint8_t def_zone[ZONE_LEN] = {
	0x01, 0x02, 0x20, 0x20, 0x00, 0x00, 0x32, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00,
};
static const uint8_t def_btn[BTN_MAP_LEN] = {
	0x01, 0x01, 0x01, 0x01, 0x02, 0x02, 0x01, 0x03, 0x04, 0x01, 0x04, 0x08,
	0x01, 0x05, 0x10, 0x07, 0x08, 0x00, 0x01, 0xe8, 0x00, 0x01, 0xe9, 0x00,
};

struct stockcfg scfg;

static const struct device *i2c = DEVICE_DT_GET(DT_NODELABEL(i2c1));
static K_MUTEX_DEFINE(ee_lock);
static bool from_eeprom;
static enum { EE_UNKNOWN, EE_BLANK, EE_VALID } ee_state;

/* saves run on their own low-priority queue: 30 ms per page must not stall USB/BT work */
K_THREAD_STACK_DEFINE(ee_wq_stack, 1024);
static struct k_work_q ee_wq;

/* pairing history (read-only for us), cached for vendor 50 00 D0 / D1 */
uint8_t stockcfg_pair_count;
uint8_t stockcfg_pair_hist[8][8];

/* stock L = 5*cnt5 + 10*cnt10, capped to what fits before the pairing history */
static uint16_t macro_len(int i)
{
	const uint8_t *h = scfg.macro_hdr[i];
	uint32_t l = 5u * sc_get16(&h[2]) + 10u * sc_get16(&h[4]);

	return MIN(l, MACRO_DATA_CAP(i));
}

int eeprom_read(uint16_t addr, void *buf, size_t len)
{
	uint8_t a[2] = {addr >> 8, addr & 0xFF};
	int err;

	k_mutex_lock(&ee_lock, K_FOREVER);
	err = i2c_write_read(i2c, EEPROM_ADDR, a, 2, buf, len);
	k_mutex_unlock(&ee_lock);
	return err;
}

int eeprom_write(uint16_t addr, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	int err = 0;

	k_mutex_lock(&ee_lock, K_FOREVER);
	while (len && !err) {
		/* never cross a page boundary inside one write */
		size_t n = MIN(len, EEPROM_PAGE - (addr % EEPROM_PAGE));
		uint8_t tx[2 + EEPROM_PAGE];

		tx[0] = addr >> 8;
		tx[1] = addr & 0xFF;
		memcpy(&tx[2], p, n);
		err = i2c_write(i2c, tx, n + 2, EEPROM_ADDR);
		k_msleep(EEPROM_WRITE_MS);
		addr += n;
		p += n;
		len -= n;
	}
	k_mutex_unlock(&ee_lock);
	return err;
}

void stockcfg_defaults(enum stockcfg_block blk)
{
	bool all = blk == BLK_ALL;

	if (all || blk == BLK_CFG) {
		memcpy(scfg.cfg, def_cfg, CFG_LEN);
		memset(scfg.custom_led, 0, sizeof(scfg.custom_led));
	}
	if (all || blk == BLK_MISC) {
		memcpy(scfg.misc, def_misc, MISC_LEN);
	}
	if (all || blk == BLK_LED) {
		memcpy(scfg.zone[0], def_zone, ZONE_LEN);
		memcpy(scfg.zone[1], def_zone, ZONE_LEN);
	}
	if (all || blk == BLK_DPI) {
		memcpy(scfg.dpi, def_dpi, DPI_LEN);
	}
	if (all || blk == BLK_BUTTONS) {
		memcpy(scfg.btn, def_btn, BTN_MAP_LEN);
	}
	if (all || blk == BLK_MACROS) {
		memset(scfg.macro_hdr, 0, sizeof(scfg.macro_hdr));
		memset(scfg.macro_data, 0, sizeof(scfg.macro_data));
	}
}

static int load_all(void)
{
	int err = 0;

	err |= eeprom_read(EE_CFG, scfg.cfg, CFG_LEN);
	err |= eeprom_read(EE_MISC, scfg.misc, MISC_LEN);
	err |= eeprom_read(EE_DPI, scfg.dpi, DPI_LEN);
	err |= eeprom_read(EE_LED, scfg.zone, sizeof(scfg.zone));
	err |= eeprom_read(EE_BUTTONS, scfg.btn, BTN_MAP_LEN);
	for (int z = 0; z < LED_ZONES; z++) {
		err |= eeprom_read(EE_CUSTOM_A(z), scfg.custom_led[z], 0x70);
		err |= eeprom_read(EE_CUSTOM_B(z), scfg.custom_led[z] + 0x70, 0x20);
	}
	for (int i = 0; i < MACROS; i++) {
		uint16_t l;

		err |= eeprom_read(EE_MACRO_HDR(i), scfg.macro_hdr[i], MACRO_HDR_LEN);
		memset(scfg.macro_data[i], 0, MACRO_DATA_LEN);	/* stock: zeros beyond L */
		l = macro_len(i);
		if (l) {
			err |= eeprom_read(EE_MACRO_DATA(i), scfg.macro_data[i], l);
		}
	}
	return err;
}

/* sanity limits so a corrupt EEPROM can't break the firmware */
static void sanitize(void)
{
	if (scfg.cfg[CFG_POLL_IDX] > 3) {
		scfg.cfg[CFG_POLL_IDX] = 3;
	}
	if (scfg.dpi[DPI_CUR] >= DPI_STAGES) {
		scfg.dpi[DPI_CUR] = 0;	/* mask 0 is legal in stock (D3 01 00 x 00) */
	}
	for (int i = 0; i < DPI_STAGES; i++) {
		uint16_t v = sc_get16(&scfg.dpi[DPI_VAL(i)]);

		if (v < 2 || v > 320) {
			memcpy(scfg.dpi, def_dpi, DPI_LEN);
			break;
		}
	}
	for (int z = 0; z < LED_ZONES; z++) {
		if (scfg.zone[z][ZONE_EFFECT] > 4 || scfg.zone[z][ZONE_BRIGHTNESS] > 100) {
			memcpy(scfg.zone[z], def_zone, ZONE_LEN);
		}
	}
	if (scfg.misc[MISC_LOWBAT_PCT] < 1 || scfg.misc[MISC_LOWBAT_PCT] > 99) {
		scfg.misc[MISC_LOWBAT_PCT] = 15;
	}
	uint16_t sn = sc_get16(&scfg.dpi[DPI_SNIPER]);

	if (sn < 2 || sn > 320) {
		sc_put16(&scfg.dpi[DPI_SNIPER], 0x10);	/* default 800 CPI */
	}
	for (int z = 0; z < LED_ZONES; z++) {
		uint8_t n = scfg.cfg[CFG_CUSTOM_CNT(z)];

		if (n > CUSTOM_LED_LEN || n % 3) {
			scfg.cfg[CFG_CUSTOM_CNT(z)] = 0;	/* would read past the arrays */
		}
	}
}

int stockcfg_init(void)
{
	uint8_t layout[4];

	k_work_queue_start(&ee_wq, ee_wq_stack, K_THREAD_STACK_SIZEOF(ee_wq_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);
	stockcfg_defaults(BLK_ALL);
	if (!device_is_ready(i2c) || eeprom_read(EE_LAYOUT, layout, sizeof(layout))) {
		LOG_WRN("EEPROM not readable, using defaults");
		ee_state = EE_UNKNOWN;
		return -EIO;
	}
	if (memcmp(layout, layout_version, sizeof(layout))) {
		/* stock rewrites defaults on mismatch; we keep them in RAM until a save */
		LOG_WRN("EEPROM layout %02x%02x%02x%02x, using defaults", layout[0], layout[1],
			layout[2], layout[3]);
		ee_state = EE_BLANK;
		return 0;
	}
	if (load_all()) {
		LOG_WRN("EEPROM read error, using defaults");
		stockcfg_defaults(BLK_ALL);
		ee_state = EE_UNKNOWN;
		return -EIO;
	}
	sanitize();
	ee_state = EE_VALID;
	from_eeprom = true;

	uint8_t cnt[4] = {0};

	if (!eeprom_read(0x1690, cnt, sizeof(cnt))) {
		stockcfg_pair_count = cnt[0];
	}
	eeprom_read(0x1490, stockcfg_pair_hist, sizeof(stockcfg_pair_hist));
	LOG_INF("settings loaded from EEPROM");
	return 0;
}

/* ---- background saves ---- */
static atomic_t pending;	/* bitmask of blocks to write */

static void write_block(int blk)
{
	switch (blk) {
	case BLK_CFG:
		eeprom_write(EE_CFG, scfg.cfg, CFG_LEN);
		for (int z = 0; z < LED_ZONES; z++) {
			eeprom_write(EE_CUSTOM_A(z), scfg.custom_led[z], 0x70);
			eeprom_write(EE_CUSTOM_B(z), scfg.custom_led[z] + 0x70, 0x20);
		}
		break;
	case BLK_MISC:
		eeprom_write(EE_MISC, scfg.misc, MISC_LEN);
		break;
	case BLK_LED:
		eeprom_write(EE_LED, scfg.zone, sizeof(scfg.zone));
		break;
	case BLK_DPI:
		eeprom_write(EE_DPI, scfg.dpi, DPI_LEN);
		break;
	case BLK_BUTTONS:
		eeprom_write(EE_BUTTONS, scfg.btn, BTN_MAP_LEN);
		break;
	case BLK_MACROS:
		for (int i = 0; i < MACROS; i++) {
			uint16_t l = macro_len(i);

			eeprom_write(EE_MACRO_HDR(i), scfg.macro_hdr[i], MACRO_HDR_LEN);
			if (l) {
				eeprom_write(EE_MACRO_DATA(i), scfg.macro_data[i], l);
			}
		}
		break;
	default:
		break;
	}
}

static void save_work_fn(struct k_work *w)
{
	uint32_t blocks = atomic_clear(&pending);

	if (!blocks) {
		return;
	}
	if (ee_state == EE_UNKNOWN) {
		uint8_t layout[4];

		if (eeprom_read(EE_LAYOUT, layout, sizeof(layout))) {
			LOG_ERR("EEPROM unreadable, save of 0x%02x dropped", blocks);
			return;
		}
		/* never overwrite a valid EEPROM we failed to load with defaults */
		ee_state = memcmp(layout, layout_version, 4) ? EE_BLANK : EE_VALID;
	}
	if (ee_state == EE_BLANK) {
		blocks = BIT_MASK(6);	/* first save on a blank EEPROM: make it consistent */
	}
	for (int b = 0; b < 6; b++) {
		if (blocks & BIT(b)) {
			write_block(b);
		}
	}
	if (ee_state == EE_BLANK) {
		/* header last: an interrupted first save stays "blank", not half-valid */
		eeprom_write(EE_FW_VERSION, fw_version, sizeof(fw_version));
		eeprom_write(EE_LAYOUT, layout_version, sizeof(layout_version));
		ee_state = EE_VALID;
	}
	LOG_INF("EEPROM saved (blocks 0x%02x)", blocks);
}
static K_WORK_DEFINE(save_work, save_work_fn);

void stockcfg_save(enum stockcfg_block blk)
{
	atomic_or(&pending, blk == BLK_ALL ? BIT_MASK(6) : BIT(blk));
	k_work_submit_to_queue(&ee_wq, &save_work);
}

/* wait for queued EEPROM writes (before reset, DFU or System OFF) */
void stockcfg_flush(void)
{
	struct k_work_sync sync;

	k_work_flush(&save_work, &sync);
	if (atomic_get(&pending)) {
		save_work_fn(&save_work);
	}
}
