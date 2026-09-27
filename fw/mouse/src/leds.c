/*
 * Two RGB zones on PWM0/PWM1 (pin map from the stock PWM table @0x646c4).
 * Stock limits the duty to ~80 %, we keep the same ceiling.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/gpio.h>

#include "pulsedart.h"

#define PWM_LED(node) PWM_DT_SPEC_GET(DT_NODELABEL(node))

static const struct pwm_dt_spec led[2][3] = {
	{PWM_LED(pwm_led0_r), PWM_LED(pwm_led0_g), PWM_LED(pwm_led0_b)},
	{PWM_LED(pwm_led1_r), PWM_LED(pwm_led1_g), PWM_LED(pwm_led1_b)},
};

static const struct gpio_dt_spec led_en =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), led_enable_gpios);

static uint8_t cache[2][3];
static bool zone_valid[2];

static void set_channel(const struct pwm_dt_spec *s, uint8_t v)
{
	/* stock: compare = 320 - v with a 320-count period, i.e. duty = v / 320 */
	uint32_t pulse = (uint64_t)s->period * v / 320;

	pwm_set_pulse_dt(s, pulse);
}

void leds_set(uint8_t zone, uint8_t r, uint8_t g, uint8_t b)
{
	if (zone > 1) {
		return;
	}
	const uint8_t v[3] = {r, g, b};

	bool any = false;

	for (int c = 0; c < 3; c++) {
		if (!zone_valid[zone] || cache[zone][c] != v[c]) {
			cache[zone][c] = v[c];
			set_channel(&led[zone][c], v[c]);
		}
	}
	zone_valid[zone] = true;
	/* LED supply (P0.24) on only while some channel is lit */
	for (int z = 0; z < 2; z++) {
		for (int c = 0; c < 3; c++) {
			any |= cache[z][c] != 0;
		}
	}
	gpio_pin_set_dt(&led_en, any);
}

void leds_set_all(uint8_t r, uint8_t g, uint8_t b)
{
	leds_set(0, r, g, b);
	leds_set(1, r, g, b);
}

void leds_off(void)
{
	leds_set_all(0, 0, 0);
}

int leds_init(void)
{
	gpio_pin_configure_dt(&led_en, GPIO_OUTPUT_INACTIVE);
	for (int z = 0; z < 2; z++) {
		for (int c = 0; c < 3; c++) {
			if (!pwm_is_ready_dt(&led[z][c])) {
				return -ENODEV;
			}
		}
	}
	leds_off();
	return 0;
}
