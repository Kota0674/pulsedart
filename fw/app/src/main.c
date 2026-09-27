/*
 * Stage 1 bring-up for the Pulsefire Dart:
 *  - proves our image boots from 0x50000 behind the stock stub and RTT works over SWD
 *  - cycles each LED colour for 2 s so the colour map from the RE can be confirmed
 *  - logs every button edge so the two side buttons can be identified physically
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bringup, LOG_LEVEL_INF);

#define SPEC(node) GPIO_DT_SPEC_GET(DT_NODELABEL(node), gpios)

struct named_pin {
	const char *name;
	struct gpio_dt_spec spec;
};

static const struct named_pin leds[] = {
	{"zone0 RED   P0.08", SPEC(led0_r)}, {"zone0 GREEN P0.04", SPEC(led0_g)},
	{"zone0 BLUE  P0.06", SPEC(led0_b)}, {"zone1 RED   P0.12", SPEC(led1_r)},
	{"zone1 GREEN P0.11", SPEC(led1_g)}, {"zone1 BLUE  P1.09", SPEC(led1_b)},
};

static const struct named_pin btns[] = {
	{"LEFT    P1.15", SPEC(btn_left)}, {"RIGHT   P1.13", SPEC(btn_right)},
	{"MIDDLE  P1.10", SPEC(btn_middle)}, {"DPI     P1.00", SPEC(btn_dpi)},
	{"BTN5/FWD P1.04", SPEC(btn_fwd)}, {"BTN4/BACK P1.02", SPEC(btn_back)},
};

static const struct gpio_dt_spec led_pwr = SPEC(led_pwr);

int main(void)
{
	LOG_INF("Pulsedart bring-up, built " __DATE__ " " __TIME__);

	gpio_pin_configure_dt(&led_pwr, GPIO_OUTPUT_ACTIVE);
	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		gpio_pin_configure_dt(&leds[i].spec, GPIO_OUTPUT_INACTIVE);
	}
	for (size_t i = 0; i < ARRAY_SIZE(btns); i++) {
		gpio_pin_configure_dt(&btns[i].spec, GPIO_INPUT);
	}

	int prev[ARRAY_SIZE(btns)] = {0};
	size_t led = 0;
	int64_t next_led = 0;

	while (1) {
		int64_t now = k_uptime_get();

		if (now >= next_led) {
			for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
				gpio_pin_set_dt(&leds[i].spec, i == led);
			}
			LOG_INF("LED on: %s", leds[led].name);
			led = (led + 1) % ARRAY_SIZE(leds);
			next_led = now + 2000;
		}

		for (size_t i = 0; i < ARRAY_SIZE(btns); i++) {
			int v = gpio_pin_get_dt(&btns[i].spec);

			if (v != prev[i]) {
				LOG_INF("BTN %s %s", btns[i].name, v ? "down" : "up");
				prev[i] = v;
			}
		}
		k_msleep(5);
	}
	return 0;
}
