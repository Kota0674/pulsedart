/*
 * PMW3389 driver. Sequence, register values and delays follow the stock firmware
 * exactly (re/sensor.md sections 1-5), except the burst address-to-data delay,
 * which is raised from the stock 10 us to the datasheet 35 us.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

#include "pulsedart.h"
#include "pmw3389_srom.h"

LOG_MODULE_REGISTER(sensor, LOG_LEVEL_INF);

#define REG_PRODUCT_ID     0x00
#define REG_MOTION         0x02
#define REG_RESOLUTION_L   0x0E
#define REG_RESOLUTION_H   0x0F
#define REG_CONFIG2        0x10
#define REG_SROM_ENABLE    0x13
#define REG_SROM_ID        0x2A
#define REG_POWER_UP_RESET 0x3A
#define REG_UNDOC_3D       0x3D	/* stock handshake after SROM, meaning unknown */
#define REG_MOTION_BURST   0x50
#define REG_SROM_LOAD_BURST 0x62

#define PRODUCT_ID_PMW3389 0x47

static const struct device *spi = DEVICE_DT_GET(DT_NODELABEL(spi0));
static const struct gpio_dt_spec ncs =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), sensor_ncs_gpios);
static const struct gpio_dt_spec pwr =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), sensor_pwr_gpios);
static const struct gpio_dt_spec motion =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), sensor_motion_gpios);

static const struct spi_config spi_cfg = {
	.frequency = 2000000,
	.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_MASTER |
		     SPI_MODE_CPOL | SPI_MODE_CPHA,
};

static bool running;

static int spi_tx(const uint8_t *buf, size_t len)
{
	const struct spi_buf b = {.buf = (void *)buf, .len = len};
	const struct spi_buf_set s = {.buffers = &b, .count = 1};

	return spi_write(spi, &spi_cfg, &s);
}

static int spi_rx(uint8_t *buf, size_t len)
{
	const struct spi_buf b = {.buf = buf, .len = len};
	const struct spi_buf_set s = {.buffers = &b, .count = 1};

	return spi_read(spi, &spi_cfg, &s);
}

static inline void ncs_low(void)  { gpio_pin_set_dt(&ncs, 1); }
static inline void ncs_high(void) { gpio_pin_set_dt(&ncs, 0); }

/* stock 0x5fe0e */
static int reg_write(uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = {reg | 0x80, val};
	int err;

	ncs_low();
	k_busy_wait(1);
	err = spi_tx(buf, sizeof(buf));
	k_busy_wait(35);	/* tSCLK-NCS (write) */
	ncs_high();
	k_busy_wait(180);	/* tSWW / tSWR */
	return err;
}

/* stock 0x5fd52 */
static int reg_read(uint8_t reg, uint8_t *val)
{
	uint8_t addr = reg & 0x7F;
	int err;

	ncs_low();
	k_busy_wait(1);
	err = spi_tx(&addr, 1);
	k_busy_wait(160);	/* tSRAD */
	if (!err) {
		err = spi_rx(val, 1);
	}
	k_busy_wait(1);
	ncs_high();
	k_busy_wait(20);	/* tSRW / tSRR */
	return err;
}

/* stock 0x5fca0 */
static int srom_download(void)
{
	static const uint8_t cmd = REG_SROM_LOAD_BURST | 0x80;
	int err;

	reg_write(REG_CONFIG2, 0x00);
	reg_write(REG_SROM_ENABLE, 0x1D);
	k_msleep(10);
	reg_write(REG_SROM_ENABLE, 0x18);

	ncs_low();
	k_busy_wait(1);
	err = spi_tx(&cmd, 1);
	k_busy_wait(140);
	for (size_t i = 0; i < sizeof(pmw3389_srom) && !err; i++) {
		err = spi_tx(&pmw3389_srom[i], 1);
		k_busy_wait(30);
	}
	ncs_high();
	k_busy_wait(200);
	return err;
}

/* stock 0x5fd8c: up to 3 attempts, then the undocumented 0x3D handshake */
static int srom_load_and_check(void)
{
	uint8_t id = 0xFF;

	for (int attempt = 0; attempt < 3; attempt++) {
		if (srom_download()) {
			continue;
		}
		reg_read(REG_SROM_ID, &id);
		if (id == PMW3389_SROM_ID) {
			break;
		}
		LOG_WRN("SROM_ID 0x%02x (attempt %d)", id, attempt + 1);
	}
	if (id != PMW3389_SROM_ID) {
		/* stock also accepts 0x00 here; we treat it as failure but carry on */
		LOG_ERR("SROM not confirmed (id 0x%02x)", id);
	}

	uint8_t v = 0;

	reg_write(REG_UNDOC_3D, 0x80);
	k_busy_wait(675);
	for (int i = 0; i < 55; i++) {
		reg_read(REG_UNDOC_3D, &v);
		if (v == 0xC0) {
			break;
		}
		k_busy_wait(625);
	}
	reg_write(REG_UNDOC_3D, 0x00);
	reg_write(REG_MOTION_BURST, 0x01);
	return id == PMW3389_SROM_ID ? 0 : -EIO;
}

int sensor_set_cpi(uint16_t cpi)
{
	uint16_t v = CLAMP(cpi / 50, 2, 320);	/* 100..16000 CPI in 50 CPI steps */

	if (!running) {
		return -EAGAIN;
	}
	/* blocking, separate writes: avoids the stock shared-buffer race (re/sensor.md 6) */
	reg_write(REG_RESOLUTION_H, v >> 8);
	reg_write(REG_RESOLUTION_L, v & 0xFF);
	reg_write(REG_MOTION_BURST, 0x01);	/* re-arm burst after non-burst access */
	return 0;
}

int sensor_init(void)
{
	uint8_t id = 0, dummy;

	if (!device_is_ready(spi)) {
		return -ENODEV;
	}
	gpio_pin_configure_dt(&ncs, GPIO_OUTPUT_INACTIVE);	/* NCS high */
	gpio_pin_configure_dt(&pwr, GPIO_OUTPUT_ACTIVE);	/* P1.07 high, as stock */
	gpio_pin_configure_dt(&motion, GPIO_INPUT);

	/*
	 * The SPIM driver moves SCK from pinctrl's low level to the mode-3 idle high
	 * level inside its first transfer. Do that transfer with NCS high, so the
	 * sensor never sees the stray edge.
	 */
	dummy = 0;
	spi_tx(&dummy, 1);
	k_msleep(5);	/* settle, in case P1.07 switches the sensor supply */

	/* datasheet power-up: NCS high -> low -> high resets the serial port */
	ncs_low();
	k_busy_wait(1);
	ncs_high();
	k_busy_wait(1);

	reg_write(REG_POWER_UP_RESET, 0x5A);
	k_msleep(50);
	reg_read(REG_PRODUCT_ID, &id);
	if (id != PRODUCT_ID_PMW3389) {
		LOG_ERR("Product_ID 0x%02x, expected 0x47", id);
		return -ENODEV;
	}
	for (uint8_t r = 0x02; r <= 0x06; r++) {
		reg_read(r, &dummy);
	}
	int err = srom_load_and_check();

	running = true;
	LOG_INF("PMW3389 up (SROM %s)", err ? "UNCONFIRMED" : "0x05");
	return err;
}

uint8_t sensor_read_reg(uint8_t reg)
{
	uint8_t v = 0;

	if (!running) {
		return 0;
	}
	reg_read(reg, &v);
	if (reg != REG_MOTION_BURST) {
		reg_write(REG_MOTION_BURST, 0x01);	/* stock 0x539ac: re-arm burst */
	}
	return v;
}

bool sensor_motion_pending(void)
{
	return gpio_pin_get_dt(&motion) == 1;
}

bool sensor_read_motion(int16_t *dx, int16_t *dy)
{
	static const uint8_t addr = REG_MOTION_BURST;
	uint8_t b[7] = {0};
	int err;

	if (!running || !sensor_motion_pending()) {
		return false;
	}
	ncs_low();
	k_busy_wait(1);
	err = spi_tx(&addr, 1);
	k_busy_wait(35);	/* tSRAD_MOTBR */
	if (!err) {
		err = spi_rx(b, sizeof(b));
	}
	ncs_high();
	k_busy_wait(20);

	if (err || !(b[0] & 0x80)) {
		return false;
	}
	*dx = (int16_t)sys_get_le16(&b[2]);
	*dy = (int16_t)sys_get_le16(&b[4]);
	return *dx || *dy;
}

/* Config2 Rest_En (bit 5). After the SROM download the sensor reads 0x20 (rest on). */
void sensor_set_rest(bool on)
{
	if (!running) {
		return;
	}
	reg_write(REG_CONFIG2, on ? 0x20 : 0x00);
	reg_write(REG_MOTION_BURST, 0x01);	/* re-arm burst after non-burst access */
}

/* stock 0x56d44: drain pending motion so the MOTION line can act as a wake source */
void sensor_prepare_sleep(void)
{
	uint8_t v;

	for (int i = 0; i < 10 && sensor_motion_pending(); i++) {
		reg_read(REG_MOTION, &v);
	}
	reg_write(REG_MOTION_BURST, 0x01);
}
