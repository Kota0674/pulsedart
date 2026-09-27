/*
 * Pulsedart firmware - shared types and module interfaces.
 * Hardware facts and their sources: PINMAP.md and the reports in re/.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/sys/util.h>

/* One HID mouse report, identical layout on USB, BLE and the stock 2.4 GHz link */
struct mouse_report {
	uint8_t buttons;	/* bit0 L, bit1 R, bit2 M, bit3 back (4), bit4 forward (5) */
	int16_t dx;
	int16_t dy;
	int8_t wheel;
};

#define BTN_LEFT    BIT(0)
#define BTN_RIGHT   BIT(1)
#define BTN_MIDDLE  BIT(2)
#define BTN_BACK    BIT(3)
#define BTN_FORWARD BIT(4)
/* Not sent to the host */
#define BTN_DPI     BIT(7)

enum wireless_mode {
	MODE_ESB = 0,	/* stock HyperX dongle (Nordic ESB) */
	MODE_BLE = 1,
};

/* ---- sensor (PMW3389) ---- */
int sensor_init(void);
int sensor_set_cpi(uint16_t cpi);
/* Returns true and the deltas since the last call if the sensor reported motion */
bool sensor_read_motion(int16_t *dx, int16_t *dy);
bool sensor_motion_pending(void);
uint8_t sensor_read_reg(uint8_t reg);	/* debug read (vendor 53 94 87), re-arms burst */
void sensor_prepare_sleep(void);
void sensor_set_rest(bool on);	/* Config2 Rest_En */

/* ---- buttons + wheel ---- */
int input_init(void);
void input_set_debounce(uint8_t press_ms, uint8_t release_ms);
/* Call every tick; elapsed_ms = tick length. Returns debounced bits (incl. BTN_DPI). */
uint8_t input_poll_buttons(uint8_t elapsed_ms);
/* Call every 1 ms. Returns wheel detents since last call (+ = up). */
int8_t input_poll_wheel(void);
void input_prepare_sleep(void);

/* ---- LEDs ---- */
int leds_init(void);
void leds_set(uint8_t zone, uint8_t r, uint8_t g, uint8_t b);
void leds_set_all(uint8_t r, uint8_t g, uint8_t b);
void leds_off(void);

/* ---- battery / charger ---- */
struct power_status {
	uint8_t soc;		/* %, 0xFF = unknown */
	uint16_t mv;		/* battery voltage, 0 = unknown */
	bool ext_power;		/* cable or Qi */
	bool charging;
	bool qi;
};
int power_init(void);
void power_update(void);	/* call ~every 5 s */
const struct power_status *power_get(void);
int power_gauge_read16(uint8_t reg, uint16_t *val);

/* ---- persistent settings ---- */
struct pd_settings {
	uint8_t mode;		/* enum wireless_mode */
	uint8_t dpi_idx;
	uint8_t esb_record[8];	/* stock pairing record format: ch, 2, addr[4], paired, 0xFF */
	bool esb_record_valid;
	uint8_t pair_on_boot;	/* start ESB pairing right after the next boot */
	bool esb_record_dirty;	/* record came from our own pairing: persist it */
};
int pd_settings_init(void);
struct pd_settings *pd_settings_get(void);
void pd_settings_save(void);

/* ---- transports ---- */
int usb_init(void);
bool usb_ready(void);
int usb_send(const struct mouse_report *r);
int usb_send_kbd(uint8_t mod, const uint8_t keys[6]);
int usb_send_consumer(uint16_t usage);
void usb_poll(void);	/* vendor requests / replies, main loop */
int usb_send_status(const uint8_t st[8]);	/* async FF 03 status on the vendor IF */
bool usb_suspended(void);

int ble_start(void);
bool ble_ready(void);
int ble_send(const struct mouse_report *r);
int ble_send_kbd(uint8_t mod, const uint8_t keys[6]);
int ble_send_consumer(uint16_t usage);
void ble_unpair_all(void);
void ble_set_battery(uint8_t soc);
void ble_activity(void);	/* restart advertising if it timed out without a host */

int esbl_start(void);
bool esbl_ready(void);
int esbl_send(const struct mouse_report *r);
int esbl_send_kbd(uint8_t mod, const uint8_t keys[6]);
int esbl_send_consumer(uint16_t usage);
void esbl_keepalive(void);
int esbl_send_status(const uint8_t st[8]);	/* async FF 03 status (0xE0 packet) */
void esbl_start_pairing(void);
bool esbl_pairing(void);
void esbl_tick_1ms(void);
void esbl_set_idle(bool idle);	/* true: radio off (stock sleep), false: back on */
#define ESB_LOST_MOUSE    BIT(0)
#define ESB_LOST_KBD      BIT(1)
#define ESB_LOST_CONSUMER BIT(2)
uint8_t esbl_take_lost(void);	/* ESB_LOST_* of reports lost since the last call */
bool esbl_take_tunnel_activity(void);
bool esbl_take_link_edge(void);	/* ESB link came (back) up since the last call */
bool esbl_take_tunnel_seen(void);	/* a host command arrived since the last call */
