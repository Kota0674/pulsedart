/*
 * Battery and charger status.
 * - TI bq27421 fuel gauge on I2C 0x55: StateOfCharge reg 0x1C, Voltage reg 0x04
 *   (re/power.md 2.2). Its data memory is RAM: it is lost when the battery is fully
 *   drained or disconnected. As stock, a background thread checks the "configured"
 *   marker after boot and re-programs the stock values (800 mAh, 3.0 V, Ra table) if needed.
 * - Charger pins confirmed live 2026-09-27: P1.11 = 0 on external power,
 *   P1.06 = 0 while charging. P0.14 is 0 on a cable (Qi-present hypothesis).
 * - P0.10: stock drives it high on a cable (likely "Qi receiver disable").
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

#include "pulsedart.h"

LOG_MODULE_REGISTER(power, LOG_LEVEL_INF);

#define BQ27421_ADDR   0x55
#define BQ_REG_VOLTAGE 0x04
#define BQ_REG_SOC     0x1C
#define BQ_REG_AVG_I   0x10

static const struct device *i2c = DEVICE_DT_GET(DT_NODELABEL(i2c1));

#define USER_GPIO(prop) GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), prop)
static const struct gpio_dt_spec pgood = USER_GPIO(chg_pgood_gpios);
static const struct gpio_dt_spec chg = USER_GPIO(chg_stat_gpios);
static const struct gpio_dt_spec qi = USER_GPIO(qi_present_gpios);
static const struct gpio_dt_spec qi_dis = USER_GPIO(qi_disable_gpios);

static struct power_status status = {.soc = 0xFF};

/* diagnostics: last 16 gauge AverageCurrent readings (signed mA; 0 below ~5 mA), SWD */
__used int16_t power_avg_ma[16];
__used uint8_t power_avg_idx;

/* 0 = OK (configured), 1 = gauge missing / wrong CHEM_ID, 2 = programmed this boot (stock) */
static uint8_t gauge_status;
static atomic_t gauge_busy;	/* set while the data memory is being programmed */
static K_MUTEX_DEFINE(bq_lock);

/* one transfer; bq27421 needs >= 66 us bus-free between packets @400 kHz */
static int bq_xfer(const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
	int err;

	k_mutex_lock(&bq_lock, K_FOREVER);
	err = rn ? i2c_write_read(i2c, BQ27421_ADDR, w, wn, r, rn)
		 : i2c_write(i2c, w, wn, BQ27421_ADDR);
	k_busy_wait(100);
	k_mutex_unlock(&bq_lock);
	return err;
}

static int bq_read16(uint8_t reg, uint16_t *val)
{
	uint8_t b[2];
	int err = bq_xfer(&reg, 1, b, 2);

	if (!err) {
		*val = sys_get_le16(b);
	}
	return err;
}

static int bq_read8(uint8_t reg, uint8_t *val)
{
	return bq_xfer(&reg, 1, val, 1);
}

/* stock write8 0x57fa4: 2-byte write, then 30 ms */
static int bq_write8(uint8_t reg, uint8_t val)
{
	uint8_t b[2] = {reg, val};
	int err = bq_xfer(b, 2, NULL, 0);

	k_msleep(30);
	return err;
}

/* stock control 0x57878: Control() = sub, result read back from 0x00 */
static int bq_control(uint16_t sub, uint16_t *val)
{
	int err = bq_write8(0x00, sub & 0xff);

	err |= bq_write8(0x01, sub >> 8);
	if (!err && val) {
		err = bq_read16(0x00, val);
	}
	return err;
}

static void bq_seal(bool unseal)
{
	if (unseal) {	/* default keys: 0x8000 twice */
		bq_control(0x8000, NULL);
		bq_control(0x8000, NULL);
	} else {
		bq_control(0x0020, NULL);
	}
}

#define BQ_FLAGS_CFGUPMODE BIT(4)

/* poll Flags.CFGUPMODE: 5 tries, 500 ms apart (stock 0x577e0 / 0x57e04) */
static bool bq_wait_cfgupmode(bool set)
{
	for (int i = 0; i < 5; i++) {
		uint16_t f;

		if (!bq_read16(0x06, &f) && !!(f & BQ_FLAGS_CFGUPMODE) == set) {
			return true;
		}
		k_msleep(500);
	}
	return false;
}

struct bq_val {
	uint8_t off;	/* offset inside the 32-byte block */
	uint16_t val;	/* written big-endian */
};

/* select class/block, patch the values, write the updated checksum (stock 0x578d0) */
static int bq_write_block(uint8_t cls, uint8_t blk, const struct bq_val *v, size_t n)
{
	uint8_t cs;
	int err = bq_write8(0x3E, cls) | bq_write8(0x3F, blk) | bq_read8(0x60, &cs);

	for (size_t i = 0; i < n && !err; i++) {
		uint8_t old[2], nw[2] = {v[i].val >> 8, v[i].val & 0xff};
		uint8_t sum = 255 - cs;

		err = bq_read8(0x40 + v[i].off, &old[0]) | bq_read8(0x41 + v[i].off, &old[1]);
		err |= bq_write8(0x40 + v[i].off, nw[0]) | bq_write8(0x41 + v[i].off, nw[1]);
		sum = sum - old[0] - old[1] + nw[0] + nw[1];
		cs = 255 - sum;
	}
	return err ? err : bq_write8(0x60, cs);
}

/* stock values (RW 0x2000261c / 0x20002638 / 0x20002654 / 0x20002674, re/power.md 2.2) */
static const struct bq_val st82_0[] = {
	{0, 0x46CD},	/* Qmax Cell 0 */
	{10, 0x0320},	/* Design Capacity 800 mAh */
	{12, 0x1360},	/* Design Energy (TI default) */
	{16, 0x0BB8},	/* Terminate Voltage 3000 mV */
};
static const struct bq_val st82_1[] = {
	{35 - 32, 0xFF5D},	/* "configured" marker checked at boot */
	{37 - 32, 0xFEF4},
	{39 - 32, 0x0002},
};
static const uint16_t ra_table[15] = {
	0x004C, 0x004C, 0x0052, 0x005F, 0x004D, 0x004B, 0x0060, 0x007C,
	0x0089, 0x008B, 0x00BC, 0x00E2, 0x01AE, 0x045A, 0x06EB,
};
static const struct bq_val st105[] = {{0, 0xFFFF}};
static const struct bq_val opconfig[] = {{0, 0x05F8}};	/* BIE cleared */

static int bq_program(void)
{
	struct bq_val ra[ARRAY_SIZE(ra_table)];
	int err;

	for (size_t i = 0; i < ARRAY_SIZE(ra); i++) {
		ra[i] = (struct bq_val){2 * i, ra_table[i]};
	}
	bq_seal(true);
	bq_control(0x0013, NULL);	/* SET_CFGUPDATE */
	if (!bq_wait_cfgupmode(true)) {
		bq_seal(false);
		return -ETIMEDOUT;
	}
	err = bq_write8(0x61, 0x00);	/* BlockDataControl: data memory access */
	err = err ? err : bq_write_block(82, 0, st82_0, ARRAY_SIZE(st82_0));
	err = err ? err : bq_write_block(82, 1, st82_1, ARRAY_SIZE(st82_1));
	err = err ? err : bq_write_block(89, 0, ra, ARRAY_SIZE(ra));
	err = err ? err : bq_write_block(105, 0, st105, ARRAY_SIZE(st105));
	err = err ? err : bq_write_block(64, 0, opconfig, ARRAY_SIZE(opconfig));
	bq_control(0x0042, NULL);	/* SOFT_RESET: leaves CFGUPDATE, applies the data */
	if (!bq_wait_cfgupmode(false) && !err) {
		err = -ETIMEDOUT;
	}
	bq_seal(false);
	bq_control(0x000D, NULL);	/* BAT_REMOVE */
	bq_control(0x000C, NULL);	/* BAT_INSERT */
	return err;
}

/* stock boot check 0x50e2c: CHEM_ID, then the State marker, then program if needed */
static void gauge_thread(void *a, void *b, void *c)
{
	uint16_t chem = 0;
	uint8_t m[2] = {0};

	if (!device_is_ready(i2c) || bq_control(0x0008, &chem) || chem != 0x0128) {
		gauge_status = 1;
		LOG_WRN("gauge: CHEM_ID 0x%04x, not a bq27421-G1A", chem);
		goto out;
	}
	bq_seal(true);
	bq_write8(0x61, 0x00);
	bq_write8(0x3E, 82);
	bq_write8(0x3F, 1);
	bq_read8(0x40 + 3, &m[0]);
	bq_read8(0x40 + 4, &m[1]);
	bq_seal(false);
	if (sys_get_be16(m) == 0xFF5D) {
		gauge_status = 0;
		LOG_INF("gauge: configured");
		goto out;
	}
	gauge_status = 2;
	LOG_WRN("gauge: marker 0x%04x, programming stock data", sys_get_be16(m));
	atomic_set(&gauge_busy, 1);
	int err = bq_program();

	LOG_INF("gauge: programmed, err %d", err);
out:
	atomic_clear(&gauge_busy);
}

/* after boot, off the input path; runs once */
K_THREAD_DEFINE(gauge_tid, 1024, gauge_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 1000);

uint8_t power_gauge_status(void)
{
	return gauge_status;
}

int power_gauge_read16(uint8_t reg, uint16_t *val)
{
	if (!device_is_ready(i2c)) {
		return -ENODEV;
	}
	return atomic_get(&gauge_busy) ? -EBUSY : bq_read16(reg, val);
}

int power_gauge_control(uint16_t sub, uint16_t *val)
{
	if (!device_is_ready(i2c)) {
		return -ENODEV;
	}
	return atomic_get(&gauge_busy) ? -EBUSY : bq_control(sub, val);
}

void power_update(void)
{
	uint16_t v;

	status.ext_power = gpio_pin_get_dt(&pgood) == 1;
	status.charging = gpio_pin_get_dt(&chg) == 1;
	status.qi = status.ext_power && gpio_pin_get_dt(&qi) == 1;

	/* same policy as stock: Qi receiver off while a cable powers the mouse */
	gpio_pin_set_dt(&qi_dis, status.ext_power && !status.qi);

	if (device_is_ready(i2c) && !atomic_get(&gauge_busy)) {
		if (!bq_read16(BQ_REG_SOC, &v) && v <= 100) {
			status.soc = v;
		}
		if (!bq_read16(BQ_REG_VOLTAGE, &v) && v > 2500 && v < 4500) {
			status.mv = v;
		}
		if (!bq_read16(BQ_REG_AVG_I, &v)) {
			power_avg_ma[power_avg_idx++ % ARRAY_SIZE(power_avg_ma)] = (int16_t)v;
		}
	}
}

const struct power_status *power_get(void)
{
	return &status;
}

int power_init(void)
{
	gpio_pin_configure_dt(&pgood, GPIO_INPUT);
	gpio_pin_configure_dt(&chg, GPIO_INPUT);
	gpio_pin_configure_dt(&qi, GPIO_INPUT);
	gpio_pin_configure_dt(&qi_dis, GPIO_OUTPUT_INACTIVE);
	power_update();
	LOG_INF("battery %u%% %u mV, ext=%d chg=%d qi=%d", status.soc, status.mv,
		status.ext_power, status.charging, status.qi);
	return 0;
}
