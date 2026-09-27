/*
 * Stock lighting engine (re/led_effects.md): per-zone effects 0 static, 1 spectrum
 * (hue cycle or c1<->c2 cross-fade), 2 breathing, 3 reactive; system indications for
 * DPI, charging, low battery, pairing. Zone 0 = rear logo, zone 1 = scroll wheel.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

void leds_fx_init(void);
void leds_fx_apply_all(void);		/* re-read zone settings from scfg */
void leds_fx_apply_zone(uint8_t z);
void leds_fx_tick(uint32_t elapsed_ms);
void leds_fx_indicate_dpi(uint8_t stage);
void leds_fx_reactive_trigger(void);	/* button press / wheel detent */
void leds_fx_set_lowbat(uint8_t level);	/* 0 none, 1 critical (<5 %), 2 low */
void leds_fx_set_charging(bool charging);
void leds_fx_set_pairing(bool on);
void leds_fx_suspend(bool off);		/* sleep / USB suspend: all dark */
void leds_fx_flash(uint8_t r, uint8_t g, uint8_t b);	/* 3 blinks, full brightness */
