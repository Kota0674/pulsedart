/*
 * Pulsedart mouse firmware - main loop.
 *
 * Settings, lighting, button actions, macros and the NGENUITY protocol follow the stock
 * firmware 1.1.0.8 (see the reports in re/) and use the stock EEPROM layout. Transports:
 * USB (four HID interfaces like stock) when a host has configured us over the cable,
 * otherwise the selected wireless mode: ESB (HyperX dongle) or Bluetooth LE.
 *
 * Button combos (hold 5 s like stock; press DPI first for the DPI+side ones):
 *   L + R + DPI     pair with a HyperX dongle (switches to ESB mode)
 *   M + DPI         factory reset of all settings (stock)
 *   DPI + Forward   toggle wireless mode ESB <-> BLE (reboots)       [ours]
 *   DPI + Back      BLE: forget all hosts and advertise              [ours]
 * MCUboot recovery (USB DFU): hold L + R + Back while the mouse powers up (fw/mcuboot_hooks).
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_wdt.h>
#include <hal/nrf_power.h>
#if CONFIG_BOOTLOADER_MCUBOOT
#include <zephyr/dfu/mcuboot.h>
#endif
#include <string.h>

#include "pulsedart.h"
#include "stockcfg.h"
#include "actions.h"
#include "leds_fx.h"
#include "vendor.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define COMBO_HOLD_MS        5000		/* stock 0x1388 scans */
#define KEEPALIVE_PERIOD_MS  4		/* stock ESB keep-alive */
/*
 * NGENUITY started while the mouse is quiet gets no FF 03 status and shows "Connection
 * lost" (it sends nothing to ask); repeat the status while the dongle link is up, until
 * the host sends its first command (then it knows we are online; a repeat would only
 * trigger more host traffic that keeps the mouse awake). Re-armed on every link edge.
 */
#define STATUS_REPEAT_MS     3000
#define POWER_POLL_MS        5000		/* stock fuel gauge poll */
#define TICK_ACTIVE_MS       1

static const uint8_t poll_div[4] = {8, 4, 2, 1};	/* stock table 0x66f20 */

static K_SEM_DEFINE(tick_sem, 0, 1);
static const struct device *wdt = DEVICE_DT_GET(DT_NODELABEL(wdt0));
static int wdt_channel = -1;

static void tick_fn(struct k_timer *t)
{
	k_sem_give(&tick_sem);
}
static K_TIMER_DEFINE(tick_timer, tick_fn, NULL);

static struct pd_settings *cfg;

/* ---------------- watchdog ---------------- */
static void watchdog_init(void)
{
	struct wdt_timeout_cfg t = {
		.window.max = 3000,
		.flags = WDT_FLAG_RESET_SOC,
	};

	if (!device_is_ready(wdt)) {
		return;
	}
	wdt_channel = wdt_install_timeout(wdt, &t);
	if (wdt_channel >= 0) {
		/* keep counting while the CPU sleeps: a hang that idles the CPU must still reset */
		wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	}
}

static void watchdog_feed(void)
{
	if (wdt_channel >= 0) {
		wdt_feed(wdt, wdt_channel);
	}
	/* the nRF52 WDT survives a soft reset (stock code may have started it): feed every RR */
	uint32_t rren = NRF_WDT->RREN;

	for (int i = 0; i < 8; i++) {
		if (rren & BIT(i)) {
			NRF_WDT->RR[i] = WDT_RR_RR_Reload;
		}
	}
}

/* ---------------- sleep: wait for a pin, like stock 0x56d28 ----------------
 * While idle nothing is polled: the tick drops to SLEEP_TICK_MS (watchdog, power) and a
 * level interrupt on MOTION, a button or a wheel contact wakes the loop.
 */
#define SLEEP_TICK_MS 1000	/* < watchdog 3 s */

static const struct gpio_dt_spec wake_pins[] = {
	GPIO_DT_SPEC_GET(DT_NODELABEL(btn_left), gpios),
	GPIO_DT_SPEC_GET(DT_NODELABEL(btn_right), gpios),
	GPIO_DT_SPEC_GET(DT_NODELABEL(btn_middle), gpios),
	GPIO_DT_SPEC_GET(DT_NODELABEL(btn_dpi), gpios),
	GPIO_DT_SPEC_GET(DT_NODELABEL(btn_fwd), gpios),
	GPIO_DT_SPEC_GET(DT_NODELABEL(btn_back), gpios),
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), sensor_motion_gpios),
};
static const struct gpio_dt_spec wake_wheel[] = {
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), wheel_a_gpios),
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), wheel_b_gpios),
};
static struct gpio_callback wake_cb[2];	/* one per GPIO port */
static atomic_t wake_irq;

static void wake_irqs_off(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(wake_pins); i++) {
		gpio_pin_interrupt_configure_dt(&wake_pins[i], GPIO_INT_DISABLE);
	}
	for (size_t i = 0; i < ARRAY_SIZE(wake_wheel); i++) {
		gpio_pin_interrupt_configure_dt(&wake_wheel[i], GPIO_INT_DISABLE);
	}
}

static void wake_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	wake_irqs_off();	/* level interrupts: once is enough */
	atomic_set(&wake_irq, 1);
	k_sem_give(&tick_sem);
}

static void sleep_init(void)
{
	const struct device *ports[2] = {NULL};
	uint32_t masks[2] = {0};

	for (size_t i = 0; i < ARRAY_SIZE(wake_pins) + ARRAY_SIZE(wake_wheel); i++) {
		const struct gpio_dt_spec *s = i < ARRAY_SIZE(wake_pins) ?
			&wake_pins[i] : &wake_wheel[i - ARRAY_SIZE(wake_pins)];
		size_t p = (ports[0] == NULL || ports[0] == s->port) ? 0 : 1;

		ports[p] = s->port;
		masks[p] |= BIT(s->pin);
	}
	for (size_t p = 0; p < 2; p++) {
		if (ports[p]) {
			gpio_init_callback(&wake_cb[p], wake_isr, masks[p]);
			gpio_add_callback(ports[p], &wake_cb[p]);
		}
	}
}

static void sleep_arm(void)
{
	atomic_clear(&wake_irq);
	sensor_prepare_sleep();	/* drain motion so MOTION goes inactive (SPI: before the lock) */

	/* arm everything before the first wake_isr can run and disarm half of it */
	unsigned int key = irq_lock();

	input_prepare_sleep();	/* wheel: pull-ups on, interrupt on the opposite level */
	for (size_t i = 0; i < ARRAY_SIZE(wake_pins); i++) {
		gpio_pin_interrupt_configure_dt(&wake_pins[i], GPIO_INT_LEVEL_ACTIVE);
	}
	irq_unlock(key);
}

/* ---------------- test mode (SWD) ----------------
 * test_cmd, written over SWD: 1 = synthetic motion (+1/-1 px alternating, sensor
 * forced to run mode, LEDs off) to measure the current of a transport without anyone
 * moving the mouse; 2 = stop; 0x10 / 0x11 = reboot into ESB / BLE.
 */
__used volatile uint8_t test_cmd;
static bool test_motion;

/* ---------------- helpers ---------------- */
static void reboot_to_mode(enum wireless_mode m)
{
	stockcfg_flush();
	cfg->mode = m;
	pd_settings_save();
	leds_off();
	k_msleep(50);
	sys_reboot(SYS_REBOOT_COLD);
}

/* debounced BTN_* bits -> stock map entries (0 L, 1 R, 2 M, 3 fwd, 4 back, 5 DPI) */
static uint8_t to_entries(uint8_t btn)
{
	return ((btn & BTN_LEFT) ? BIT(MAP_LEFT) : 0) |
	       ((btn & BTN_RIGHT) ? BIT(MAP_RIGHT) : 0) |
	       ((btn & BTN_MIDDLE) ? BIT(MAP_MIDDLE) : 0) |
	       ((btn & BTN_FORWARD) ? BIT(MAP_FORWARD) : 0) |
	       ((btn & BTN_BACK) ? BIT(MAP_BACK) : 0) |
	       ((btn & BTN_DPI) ? BIT(MAP_DPI) : 0);
}

/* ---------------- combos ---------------- */
#define E(x) BIT(MAP_##x)
static const uint8_t combo_pair = E(LEFT) | E(RIGHT) | E(DPI);
static const uint8_t combo_reset = E(MIDDLE) | E(DPI);
static const uint8_t combo_mode = E(DPI) | E(FORWARD);
static const uint8_t combo_unpair = E(DPI) | E(BACK);

static void status_factory_reset(void);

static void factory_reset(void)
{
	LOG_INF("factory reset");
	stockcfg_defaults(BLK_ALL);
	actions_reset();
	stockcfg_save(BLK_ALL);
	leds_fx_apply_all();
	dpi_apply();
	leds_fx_flash(0xFF, 0xFF, 0xFF);
	status_factory_reset();
}

/*
 * Returns the entries to hide from the button actions. Entries pressed while DPI is
 * held are combo keys (DPI itself too), so DPI's own action does not fire; a button
 * that was already held before DPI (a drag) keeps working.
 */
static uint8_t handle_combos(uint8_t ent, uint8_t prev_ent, int64_t now)
{
	static uint8_t held;
	static int64_t since;
	static bool fired;
	static uint8_t hidden;
	static bool armed;	/* combos act only after all buttons were released once */
	uint8_t pressed = ent & ~prev_ent;

	/* a combo still held across its own reboot must not fire again */
	if (!ent) {
		armed = true;
	}

	if (ent != held) {
		held = ent;
		since = now;
		fired = false;
	}
	const uint8_t *dm = &scfg.btn[MAP_DPI * 3];
	/* DPI is a combo modifier only while it is a DPI-step key (no press action) */
	bool dpi_mod = dm[0] == 7 && dm[1] >= 6 && dm[1] <= 9;

	if (dpi_mod && (ent & E(DPI))) {
		hidden |= pressed & ~E(DPI);
		if (hidden) {
			hidden |= E(DPI);
		}
		if ((ent & (E(LEFT) | E(RIGHT))) == (E(LEFT) | E(RIGHT))) {
			hidden |= E(LEFT) | E(RIGHT) | E(DPI);
		}
	}
	hidden &= ent;

	if (armed && !fired && now - since >= COMBO_HOLD_MS) {
		if (ent == combo_pair) {
			fired = true;
			if (cfg->mode != MODE_ESB) {
				cfg->pair_on_boot = 1;
				reboot_to_mode(MODE_ESB);
			}
			esbl_start_pairing();
		} else if (ent == combo_reset) {
			fired = true;
			factory_reset();
		} else if (ent == combo_mode) {
			fired = true;
			reboot_to_mode(cfg->mode == MODE_ESB ? MODE_BLE : MODE_ESB);
		} else if (ent == combo_unpair && cfg->mode == MODE_BLE) {
			fired = true;
			ble_unpair_all();
			leds_fx_flash(0, 0, 0xFF);
		}
	}
	if (fired) {
		/* stock: combo keys are released on the host and swallowed until let go */
		hidden |= ent;
	}
	return hidden;
}

/* ---------------- power / battery indications ---------------- */
static void power_poll(void)
{
	const struct power_status *p = power_get();
	uint8_t level = 0;

	power_update();
	if (!p->ext_power && p->soc <= 100) {
		if (p->soc < 5) {
			level = 1;
		} else if (p->soc <= scfg.misc[MISC_LOWBAT_PCT]) {
			level = 2;
		}
	}
	leds_fx_set_lowbat(level);
	leds_fx_set_charging(p->ext_power && p->charging);
	if (cfg->mode == MODE_BLE) {
		ble_set_battery(p->soc);
	}
}

/* ---------------- report sending ---------------- */
static int send_mouse(const struct mouse_report *r)
{
	if (usb_ready()) {
		return usb_send(r);
	}
	return cfg->mode == MODE_BLE ? ble_send(r) : esbl_send(r);
}

static int send_kbd(uint8_t mod, const uint8_t keys[6])
{
	if (usb_ready()) {
		return usb_send_kbd(mod, keys);
	}
	return cfg->mode == MODE_BLE ? ble_send_kbd(mod, keys) : esbl_send_kbd(mod, keys);
}

static int send_consumer(uint16_t usage)
{
	if (usb_ready()) {
		return usb_send_consumer(usage);
	}
	return cfg->mode == MODE_BLE ? ble_send_consumer(usage) : esbl_send_consumer(usage);
}

static inline bool retry(int err)
{
	return err == -EBUSY || err == -ENOMEM;
}

/* ---------------- async status FF 03 (re/status_ff03.md) ----------------
 * [2] DPI stage, [3] power state, [4] awake (NGENUITY's "wireless mouse online"),
 * [5] sniper held, [6] factory reset done, [7] held buttons 0..5. Armed by button
 * edges (4), wake / wireless boot / power change / factory reset (2); counts down on
 * ticks that send no other report, then goes to the host on USB IF1 or as a 0xE0
 * packet through the dongle.
 */
#define STATUS_GIVE_UP_MS 1000

static uint8_t st[8] = {0xFF, 0x03};
static uint8_t st_countdown;
static bool st_pending;
static uint32_t st_wait_ms;

static uint8_t power_state(void)
{
	const struct power_status *p = power_get();

	return p->ext_power ? (p->charging ? 1 : 3) : 0;	/* same encoding as 51 00 */
}

static void status_arm(uint8_t n)
{
	if (!esbl_pairing()) {	/* stock: no status while pairing */
		st_countdown = n;
		st_pending = false;
	}
}

static void status_factory_reset(void)
{
	st[6] = 1;
	st_countdown = 2;	/* stock 0x5aa28: also while pairing */
	st_pending = false;
}

/*
 * External power keeps the mouse awake like stock: a cable does, a Qi pad does not
 * (stock idles when powered with P0.14 = 1, re/power.md 5.3).
 */
static bool power_keeps_awake(void)
{
	const struct power_status *p = power_get();

	return p->ext_power && !p->qi;
}

/* returns true if the status went out this tick */
static bool status_tick(bool other_sent, bool awake, uint8_t ent, uint32_t dt)
{
	if (esbl_pairing()) {
		return false;	/* stock: the countdown is frozen while pairing */
	}
	if (st_countdown && !other_sent && --st_countdown == 0) {
		st_pending = true;
		st_wait_ms = 0;
	}
	if (!st_pending || other_sent) {
		return false;
	}
	st[2] = dpi_current();
	st[3] = power_state();
	st[4] = awake;
	st[5] = dpi_sniper_active();
	st[7] = ent & 0x3F;

	int e = usb_ready()		 ? usb_send_status(st)
		: cfg->mode == MODE_ESB ? esbl_send_status(st)
					 : -ENOTSUP;	/* BLE: stock has no such path */

	if (e == 0) {
		st[6] = 0;
		st_pending = false;
		return true;
	}
	if ((e != -EBUSY && e != -EAGAIN) || (st_wait_ms += dt) >= STATUS_GIVE_UP_MS) {
		st_pending = false;
	}
	return false;
}

int main(void)
{
	struct host_state hs = {0};
	struct mouse_report acc = {0};
	uint8_t sent_mbtn = 0, sent_kmod = 0, sent_keys[6] = {0};
	uint16_t sent_consumer = 0;
	bool kbd_force = false, consumer_force = false;
	uint8_t prev_ent = 0;
	int64_t last_activity, last_power_poll = -POWER_POLL_MS;
	uint32_t ms_since_send = 0, ms_since_report = 0, ms_since_status = 0;
	bool host_talking = false, usb_was = false, leds_off_now = false;
	bool idle = false;
	int err;

	/* log and clear the reset reason (the register accumulates across resets) */
	uint32_t reas = NRF_POWER->RESETREAS;

	NRF_POWER->RESETREAS = reas;
	LOG_INF("Pulsedart firmware " __DATE__ " " __TIME__ ", reset reason 0x%05x%s%s%s%s", reas,
		(reas & POWER_RESETREAS_DOG_Msk) ? " WATCHDOG" : "",
		(reas & POWER_RESETREAS_SREQ_Msk) ? " soft" : "",
		(reas & POWER_RESETREAS_OFF_Msk) ? " wake-from-OFF" : "",
		(reas & POWER_RESETREAS_LOCKUP_Msk) ? " LOCKUP" : "");
	watchdog_init();
#if CONFIG_BOOTLOADER_MCUBOOT
	/* overwrite-only MCUboot needs no confirmation; harmless if already confirmed */
	boot_write_img_confirmed();
#endif

	pd_settings_init();
	cfg = pd_settings_get();
	leds_init();
	power_init();
	stockcfg_init();

	/* stock boot overrides (not written back): debounce 5/18, idle 10.5 s / 60 s */
	scfg.cfg[CFG_DEBOUNCE_PRESS] = 5;
	scfg.cfg[CFG_DEBOUNCE_RELEASE] = 18;
	sys_put_le32(10500, &scfg.cfg[CFG_IDLE_T1]);
	sys_put_le32(60000, &scfg.cfg[CFG_IDLE_T2]);

	input_init();
	sleep_init();
	leds_fx_init();

	err = sensor_init();
	if (err) {
		LOG_ERR("sensor init %d", err);
	}
	actions_init();

	err = usb_init();
	if (err) {
		LOG_ERR("USB init %d", err);
	}

	if (cfg->mode == MODE_BLE) {
		err = ble_start();
		ble_set_battery(power_get()->soc);
		leds_fx_flash(0x00, 0x00, 0xFF);
	} else {
		err = esbl_start();
		leds_fx_flash(0x00, 0xFF, 0x00);
		if (cfg->pair_on_boot) {
			cfg->pair_on_boot = 0;
			pd_settings_save();
			esbl_start_pairing();
		}
	}
	if (err) {
		LOG_ERR("wireless start %d", err);
	}
	status_arm(2);	/* stock: RW default countdown + "became active" at boot */
	uint8_t prev_pwr = power_state();


	last_activity = k_uptime_get();
	k_timer_start(&tick_timer, K_MSEC(TICK_ACTIVE_MS), K_MSEC(TICK_ACTIVE_MS));
	int64_t prev_tick = k_uptime_get();

	while (1) {
		k_sem_take(&tick_sem, K_FOREVER);
		watchdog_feed();
		int64_t now = k_uptime_get();
		/* measured, so a slow iteration does not slow debounce, macros, LEDs */
		uint32_t dt = (uint32_t)CLAMP(now - prev_tick, 1, 100);

		prev_tick = now;

		if (test_cmd) {
			uint8_t c = test_cmd;

			test_cmd = 0;
			if (c == 1 || c == 2) {
				test_motion = c == 1;
				if (test_motion) {
					atomic_set(&wake_irq, 1);	/* leave sleep */
				}
				sensor_set_rest(!test_motion);
				LOG_INF("test motion %s", test_motion ? "on" : "off");
			} else if (c == 0x10 || c == 0x11) {
				reboot_to_mode(c == 0x10 ? MODE_ESB : MODE_BLE);
			}
		}
		/* ---- sleep (stock WFE loop): nothing is polled until a wake pin fires ---- */
		bool woke = false;

		if (idle) {
			if (!atomic_clear(&wake_irq) && !usb_ready()) {
				if (now - last_power_poll >= POWER_POLL_MS) {
					last_power_poll = now;
					power_poll();
				}
				if (!power_keeps_awake()) {	/* cable power: 5 s poll (stock: P1.06 pin) */
					continue;
				}
			}
			wake_irqs_off();
			woke = true;
			dt = 1;	/* not a 100 ms step: keep debounce and timing normal */
		}

		/* ---- inputs ---- */
		input_set_debounce(scfg.cfg[CFG_DEBOUNCE_PRESS], scfg.cfg[CFG_DEBOUNCE_RELEASE]);
		uint8_t ent = to_entries(input_poll_buttons(dt));
		int8_t wheel = input_poll_wheel();
		int16_t dx, dy;
		bool moved = sensor_read_motion(&dx, &dy);

		if (test_motion) {
			static int8_t sign = 1;

			dx = sign;
			dy = 0;
			sign = -sign;
			moved = true;
		}

		if (moved) {
			acc.dx = CLAMP(acc.dx + dx, INT16_MIN + 1, INT16_MAX);
			acc.dy = CLAMP(acc.dy + dy, INT16_MIN + 1, INT16_MAX);
		}
		if (moved || wheel || ent || actions_busy() || esbl_take_tunnel_activity() || woke) {
			last_activity = now;
			if (cfg->mode == MODE_BLE) {
				ble_activity();
			}
			if (idle) {
				idle = false;
				k_timer_start(&tick_timer, K_MSEC(TICK_ACTIVE_MS),
					      K_MSEC(TICK_ACTIVE_MS));
				esbl_set_idle(false);
				status_arm(2);	/* stock 0x5aea6: tells NGENUITY we are back */
				LOG_INF("wake from idle");
			}
		}

		/* ---- combos, actions, macros ---- */
		actions_set_suppressed(handle_combos(ent, prev_ent, now));
		if (ent != prev_ent) {
			status_arm(4);	/* stock 0x58426: every physical press / release */
		}
		prev_ent = ent;
		actions_tick(ent, wheel, dt, &hs);

		esbl_tick_1ms();
		if (esbl_take_tunnel_seen()) {
			host_talking = true;	/* NGENUITY knows we are online: no heartbeat needed */
		}
		bool usb_now = usb_ready();

		if (usb_was && !usb_now) {
			status_arm(2);	/* back on wireless after a cable session */
		}
		usb_was = usb_now;
		if (esbl_take_link_edge()) {
			status_arm(50);	/* let the dongle settle its link-alive first */
			ms_since_status = 0;
			host_talking = false;
		} else if (cfg->mode == MODE_ESB && !usb_now && esbl_ready() && !host_talking &&
			   (ms_since_status += dt) >= STATUS_REPEAT_MS) {
			ms_since_status = 0;
			if (!st_countdown && !st_pending) {
				status_arm(1);
			}
		}
		usb_poll();
		vendor_poll();

		/* ---- reports ---- */
		int sent = -EAGAIN;

		/* a lost 0x60/0x61/0x62: resend that state even if nothing changed */
		if (cfg->mode == MODE_ESB) {
			uint8_t lost = esbl_take_lost();

			if (lost & ESB_LOST_MOUSE) {
				sent_mbtn = 0xFF;
			}
			kbd_force |= !!(lost & ESB_LOST_KBD);
			consumer_force |= !!(lost & ESB_LOST_CONSUMER);
		}

		/* keyboard / consumer: on change */
		if (kbd_force || hs.kmod != sent_kmod || memcmp(hs.keys, sent_keys, 6)) {
			int e = send_kbd(hs.kmod, hs.keys);

			if (!retry(e)) {
				sent_kmod = hs.kmod;
				memcpy(sent_keys, hs.keys, 6);
				kbd_force = false;
			}
			if (!e) {
				sent = 0;
			}
		} else if (consumer_force || hs.consumer != sent_consumer) {
			int e = send_consumer(hs.consumer);

			if (!retry(e)) {
				sent_consumer = hs.consumer;
				consumer_force = false;
			}
			if (!e) {
				sent = 0;
			}
		}

		/* mouse: at the selected polling rate (stock divider 8/4/2/1 ms) */
		acc.buttons = hs.mbtn;
		acc.wheel = hs.wheel;
		ms_since_report += dt;
		bool dirty = acc.dx || acc.dy || acc.wheel || acc.buttons != sent_mbtn;

		if (dirty && sent != 0 &&
		    ms_since_report >= poll_div[scfg.cfg[CFG_POLL_IDX] & 3]) {
			sent = send_mouse(&acc);
			if (sent == 0) {
				sent_mbtn = acc.buttons;
				ms_since_report = 0;
			}
			/* keep motion only while the link is merely busy; never replay stale motion */
			if (!retry(sent)) {
				acc.dx = acc.dy = 0;
				hs.wheel = 0;
			}
		}
		/* macro handshake: the next event waits until this one reached the host */
		actions_host_synced(hs.mbtn == sent_mbtn && hs.wheel == 0,
				    !kbd_force && hs.kmod == sent_kmod && !memcmp(hs.keys, sent_keys, 6));

		/* awake: stock 0x5aea2 / 0x5aec2 / 0x5a50e */
		if (status_tick(sent == 0, !idle && !usb_ready(), ent, dt)) {
			sent = 0;
		}

		if (sent == 0) {
			ms_since_send = 0;
		} else if ((ms_since_send += dt) >= KEEPALIVE_PERIOD_MS && cfg->mode == MODE_ESB &&
			   !idle && !usb_ready()) {
			esbl_keepalive();
			ms_since_send = 0;
		}

		/* ---- power, LEDs ---- */
		if (now - last_power_poll >= POWER_POLL_MS) {
			last_power_poll = now;
			power_poll();
			if (power_state() != prev_pwr) {
				prev_pwr = power_state();
				status_arm(2);	/* stock 0x5a55e */
			}
		}
		leds_fx_set_pairing(esbl_pairing());
		/* stock: LEDs off when the host sleeps and when the mouse idles on battery */
		bool leds_off_want = usb_suspended() || idle || test_motion;

		if (leds_off_want != leds_off_now) {
			leds_fx_suspend(leds_off_want);
			leds_off_now = leds_off_want;
		}
		leds_fx_tick(dt);

		/* ---- idle / sleep ---- */
		if (power_keeps_awake() || usb_ready() || esbl_pairing()) {
			last_activity = now;
		}
		if (!idle && now - last_activity >= sys_get_le32(&scfg.cfg[CFG_IDLE_T1])) {
			idle = true;
			/* stock 0x5ae58 / 0x56d28: keep-alives stop and the radio goes off */
			esbl_set_idle(true);
			leds_fx_suspend(true);
			leds_off_now = true;
			k_timer_start(&tick_timer, K_MSEC(SLEEP_TICK_MS), K_MSEC(SLEEP_TICK_MS));
			sleep_arm();
			LOG_INF("idle");
		}
	}
	return 0;
}
