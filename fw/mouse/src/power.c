/*
 * Battery and charger status.
 * - TI bq27421 fuel gauge on I2C 0x55: StateOfCharge reg 0x1C, Voltage reg 0x04
 *   (re/power.md 2.2). The stock firmware programs the gauge once; its data memory
 *   survives while the battery stays connected, so we only read it.
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

static int bq_read16(uint8_t reg, uint16_t *val)
{
	uint8_t b[2];
	int err = i2c_write_read(i2c, BQ27421_ADDR, &reg, 1, b, 2);

	if (!err) {
		*val = sys_get_le16(b);
	}
	return err;
}

int power_gauge_read16(uint8_t reg, uint16_t *val)
{
	return device_is_ready(i2c) ? bq_read16(reg, val) : -ENODEV;
}

void power_update(void)
{
	uint16_t v;

	status.ext_power = gpio_pin_get_dt(&pgood) == 1;
	status.charging = gpio_pin_get_dt(&chg) == 1;
	status.qi = status.ext_power && gpio_pin_get_dt(&qi) == 1;

	/* same policy as stock: Qi receiver off while a cable powers the mouse */
	gpio_pin_set_dt(&qi_dis, status.ext_power && !status.qi);

	if (device_is_ready(i2c)) {
		if (!bq_read16(BQ_REG_SOC, &v) && v <= 100) {
			status.soc = v;
		}
		k_busy_wait(100);	/* bq27421: >= 66 us bus-free between packets @400 kHz */
		if (!bq_read16(BQ_REG_VOLTAGE, &v) && v > 2500 && v < 4500) {
			status.mv = v;
		}
		k_busy_wait(100);
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
