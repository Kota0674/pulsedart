/*
 * Buttons and scroll wheel, polled every 1 ms like the stock firmware.
 * Debounce: 5 ms press / 18 ms release (stock values set at boot, re/input_usb_led.md).
 * Wheel: the stock firmware enables the encoder pull-ups only while sampling to
 * avoid a constant current through a contact that rests closed; we do the same.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "pulsedart.h"

LOG_MODULE_REGISTER(input, LOG_LEVEL_INF);

static uint8_t debounce_press_ms = 5;	/* stock boot values; NGENUITY D0 54 78 */
static uint8_t debounce_release_ms = 18;

void input_set_debounce(uint8_t press_ms, uint8_t release_ms)
{
	debounce_press_ms = MAX(press_ms, 1);
	debounce_release_ms = MAX(release_ms, 1);
}

/* Wheel direction is not confirmed on hardware yet (re/input_usb_led.md); flip here */
#define WHEEL_INVERT 0

#define BTN(node) GPIO_DT_SPEC_GET(DT_NODELABEL(node), gpios)

static const struct {
	struct gpio_dt_spec spec;
	uint8_t bit;
} buttons[] = {
	{BTN(btn_left), BTN_LEFT},     {BTN(btn_right), BTN_RIGHT},
	{BTN(btn_middle), BTN_MIDDLE}, {BTN(btn_back), BTN_BACK},
	{BTN(btn_fwd), BTN_FORWARD},   {BTN(btn_dpi), BTN_DPI},
};

static const struct gpio_dt_spec wheel_a =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), wheel_a_gpios);
static const struct gpio_dt_spec wheel_b =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), wheel_b_gpios);

static uint8_t state;
static uint8_t counter[ARRAY_SIZE(buttons)];
static uint8_t wheel_prev;
static int8_t wheel_acc;

int input_init(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
		gpio_pin_configure_dt(&buttons[i].spec, GPIO_INPUT);
	}
	gpio_pin_configure_dt(&wheel_a, GPIO_DISCONNECTED);
	gpio_pin_configure_dt(&wheel_b, GPIO_DISCONNECTED);
	return 0;
}

uint8_t input_poll_buttons(uint8_t elapsed_ms)
{
	for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
		bool raw = gpio_pin_get_dt(&buttons[i].spec) == 1;
		bool cur = state & buttons[i].bit;

		if (raw == cur) {
			counter[i] = 0;
			continue;
		}
		counter[i] = MIN(counter[i] + elapsed_ms, UINT8_MAX);
		if (counter[i] >= (raw ? debounce_press_ms : debounce_release_ms)) {
			state ^= buttons[i].bit;
			counter[i] = 0;
		}
	}
	return state;
}

static uint8_t wheel_sample(void)
{
	gpio_pin_configure_dt(&wheel_a, GPIO_INPUT | GPIO_PULL_UP);
	gpio_pin_configure_dt(&wheel_b, GPIO_INPUT | GPIO_PULL_UP);
	k_busy_wait(10);	/* stock: ~10 us */
	/* stock bit order: A in bit 0, B in bit 1 */
	uint8_t s = (gpio_pin_get_dt(&wheel_b) << 1) | gpio_pin_get_dt(&wheel_a);

	/* disconnected, not floating inputs, between samples (stock 0x5c930) */
	gpio_pin_configure_dt(&wheel_a, GPIO_DISCONNECTED);
	gpio_pin_configure_dt(&wheel_b, GPIO_DISCONNECTED);
	return s;
}

int8_t input_poll_wheel(void)
{
	/* Gray-code transition table: +1 / -1 per valid step, 0 for none/invalid */
	static const int8_t table[16] = {
		0, -1, 1, 0,
		1, 0, 0, -1,
		-1, 0, 0, 1,
		0, 1, -1, 0,
	};
	uint8_t s = wheel_sample();

	wheel_acc += table[(wheel_prev << 2) | s];
	wheel_prev = s;

	/* one detent = two valid transitions (stock) */
	int8_t out = wheel_acc / 2;

	wheel_acc -= out * 2;
	return WHEEL_INVERT ? -out : out;
}

/* stock 0x56d96: wheel pull-ups on, SENSE opposite to the current level, so a step wakes */
void input_prepare_sleep(void)
{
	const struct gpio_dt_spec *w[] = {&wheel_a, &wheel_b};

	for (size_t i = 0; i < ARRAY_SIZE(w); i++) {
		gpio_pin_configure_dt(w[i], GPIO_INPUT | GPIO_PULL_UP);
	}
	k_busy_wait(10);
	for (size_t i = 0; i < ARRAY_SIZE(w); i++) {
		gpio_pin_interrupt_configure_dt(w[i], gpio_pin_get_raw(w[i]->port, w[i]->pin) ?
						      GPIO_INT_LEVEL_LOW : GPIO_INT_LEVEL_HIGH);
	}
}
