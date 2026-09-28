/*
 * MCUboot action hooks for the Pulsedart mouse (built into the MCUboot image only).
 *
 * Recovery gesture (one hand): hold Left + Right + Back (rear side button, thumb) while the
 * mouse powers up, and keep holding for 3 s. MCUboot then enters USB DFU
 * (CONFIG_BOOT_USB_DFU_GPIO with mcuboot-button0 = Back), and the LEDs turn blue. Flash a signed image with
 * dfu-util; MCUboot checks it and boots it.
 *
 * MCUboot itself only supports a single detect pin. This hook runs first (STARTUP) and
 * decides: if Back is held but the full combo is not, it waits until Back is released,
 * so that MCUboot's own single-pin check sees "not pressed" and boots normally.
 *
 * The nRF52 watchdog keeps running across a soft reset (our application starts it with
 * 3 s), and MCUboot does not feed it while it waits for DFU, so a timer feeds it here.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <cmsis_core.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_wdt.h>
#include <bootutil/mcuboot_status.h>

#define COMBO_HOLD_MS 3000

static const struct gpio_dt_spec btn_l = GPIO_DT_SPEC_GET(DT_NODELABEL(btn_left), gpios);
static const struct gpio_dt_spec btn_r = GPIO_DT_SPEC_GET(DT_NODELABEL(btn_right), gpios);
static const struct gpio_dt_spec btn_b = GPIO_DT_SPEC_GET(DT_NODELABEL(btn_back), gpios);

/* LED supply enable P0.24, blue of both zones: logo P0.06, wheel P1.09 (PINMAP.md) */
#define LED_EN     NRF_GPIO_PIN_MAP(0, 24)
#define LED_LOGO_B NRF_GPIO_PIN_MAP(0, 6)
#define LED_WHEEL_B NRF_GPIO_PIN_MAP(1, 9)

static void wdt_feed(void)
{
	uint32_t rren = NRF_WDT->RREN;

	for (int i = 0; i < 8; i++) {
		if (rren & BIT(i)) {
			NRF_WDT->RR[i] = WDT_RR_RR_Reload;
		}
	}
}

static void feeder_fn(struct k_timer *t)
{
	wdt_feed();
}
static K_TIMER_DEFINE(feeder, feeder_fn, NULL);
static bool recovery;

static bool held(const struct gpio_dt_spec *b)
{
	return gpio_pin_get_dt(b) == 1;
}

static void leds_blue(void)
{
	nrf_gpio_cfg_output(LED_EN);
	nrf_gpio_cfg_output(LED_LOGO_B);
	nrf_gpio_cfg_output(LED_WHEEL_B);
	nrf_gpio_pin_set(LED_EN);
	nrf_gpio_pin_set(LED_LOGO_B);
	nrf_gpio_pin_set(LED_WHEEL_B);
}

static void check_recovery_combo(void)
{
	const struct gpio_dt_spec *b[] = {&btn_l, &btn_r, &btn_b};

	for (size_t i = 0; i < ARRAY_SIZE(b); i++) {
		gpio_pin_configure_dt(b[i], GPIO_INPUT);
	}
	if (!held(&btn_b)) {
		return;		/* the normal case: boot without delay */
	}

	int64_t since = 0;

	for (;;) {
		wdt_feed();
		if (!held(&btn_b)) {
			return;	/* Back alone (or released early): normal boot */
		}
		bool combo = held(&btn_l) && held(&btn_r);
		int64_t now = k_uptime_get();

		if (!combo) {
			since = 0;
		} else if (!since) {
			since = now;
		} else if (now - since >= COMBO_HOLD_MS) {
			/* recovery: Back is still held, so MCUboot's pin check enters USB DFU */
			leds_blue();
			k_timer_start(&feeder, K_MSEC(500), K_MSEC(500));
			recovery = true;
			return;
		}
		k_msleep(10);
	}
}

void mcuboot_status_change(mcuboot_status_type_t status)
{
	switch (status) {
	case MCUBOOT_STATUS_STARTUP:
		wdt_feed();
		check_recovery_combo();
		break;
	case MCUBOOT_STATUS_USB_DFU_ENTERED:
		leds_blue();
		break;
	case MCUBOOT_STATUS_BOOTABLE_IMAGE_FOUND:
		k_timer_stop(&feeder);	/* the application feeds it from here on */
		if (recovery) {
			/*
			 * The recovery USB stack is still up; an application started on top of it
			 * does not see the cable. The new image is installed by now, so reboot:
			 * the next pass boots it cleanly (buttons released, no USB in MCUboot).
			 */
			NVIC_SystemReset();	/* CONFIG_REBOOT is off in MCUboot */
		}
		break;
	default:
		break;
	}
}
