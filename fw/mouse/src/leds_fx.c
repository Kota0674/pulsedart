/*
 * Stock-equivalent lighting (re/led_effects.md), driven from the main loop tick.
 * The user effect keeps running underneath every indication, so it resumes with its
 * own speed and phase (fixes the stock restore quirks listed in led_effects.md 4.1/4.2).
 */
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <string.h>

#include "pulsedart.h"
#include "stockcfg.h"
#include "leds_fx.h"
#include "led_tables.h"

#ifndef ABS
#define ABS(x) ((x) < 0 ? -(x) : (x))
#endif
#define ENV_LAST        187	/* led_breath[0..187] */
#define DPI_STEP_MS     30	/* stock 0x3d7 ticks */
#define DPI_BRI         50
#define CHG_STEP_MS     30
#define CHG_BRI         50
#define LOWBAT_BRI      50

enum zmode { Z_STATIC, Z_HUE, Z_XFADE, Z_BREATH, Z_REACTIVE, Z_DARK };

struct zone {
	enum zmode mode;
	uint8_t bri;
	uint8_t col[3];
	uint8_t tgt[3];
	uint8_t flag;
	uint16_t idx;
	uint16_t last;
	uint16_t period_ms;
	int32_t acc_ms;
	bool running;		/* reactive: pulse in progress */
	uint8_t out[3];		/* current colour before brightness */
};

static struct zone zs[LED_ZONES];

/* indications */
static struct {
	bool on;
	uint8_t rgb[3];
	uint16_t idx;
	int32_t acc;
} dpi_ind;
static struct {
	bool on;
	uint16_t idx;
	int32_t acc;
} chg;
static uint8_t lowbat;
static bool lowbat_phase;
static int32_t lowbat_acc;
static bool pairing;
static bool suspended;
/* mode / reset indication: 3 blinks at full brightness */
#define FLASH_BLINKS  3
#define FLASH_HALF_MS 200
static int64_t flash_start = -1;
static uint8_t flash_rgb[3];

/* ---- output: stock set_zone 0x64428 (brightness 0..100 -> x256/101) ---- */
static void set_zone(uint8_t z, uint8_t bri, const uint8_t rgb[3])
{
	uint16_t scale = (uint16_t)((bri << 8) / 101);

	leds_set(z, (rgb[0] * scale) >> 8, (rgb[1] * scale) >> 8, (rgb[2] * scale) >> 8);
}

/* stock HSV -> RGB with gamma, 0x59d9c */
static void hsv2rgb(uint8_t h, uint8_t s, uint8_t v, uint8_t out[3])
{
	const uint8_t *g = led_gamma;

	if (s == 0) {
		out[0] = out[1] = out[2] = g[v];
		return;
	}
	uint8_t region = h / 43;
	uint16_t rem = (h - region * 43) * 6;
	uint8_t p = (v * (255 - s)) >> 8;
	uint8_t q = (v * (255 - ((s * rem) >> 8))) >> 8;
	uint8_t t = (v * (255 - ((s * (255 - rem)) >> 8))) >> 8;

	switch (region) {
	case 0: out[0] = g[v]; out[1] = g[t]; out[2] = g[p]; break;
	case 1: out[0] = g[q]; out[1] = g[v]; out[2] = g[p]; break;
	case 2: out[0] = g[p]; out[1] = g[v]; out[2] = g[t]; break;
	case 3: out[0] = g[p]; out[1] = g[q]; out[2] = g[v]; break;
	case 4: out[0] = g[t]; out[1] = g[p]; out[2] = g[v]; break;
	default: out[0] = g[v]; out[1] = g[p]; out[2] = g[q]; break;
	}
}

static void scale_env(const uint8_t in[3], uint16_t idx, uint8_t out[3])
{
	uint8_t e = led_breath[MIN(idx, ENV_LAST)];

	for (int c = 0; c < 3; c++) {
		out[c] = (in[c] * e) >> 8;
	}
}

/* ---- user effects ---- */
void leds_fx_apply_zone(uint8_t z)
{
	const uint8_t *b = scfg.zone[z];
	struct zone *s = &zs[z];

	if (b[ZONE_EFFECT] > 3) {
		return;	/* effect 4 (custom array): stock keeps the previous effect running */
	}
	memset(s, 0, sizeof(*s));
	s->bri = b[ZONE_BRIGHTNESS];
	s->period_ms = b[ZONE_SPEED] + 16;

	switch (b[ZONE_EFFECT]) {
	case 0:
		s->mode = Z_STATIC;
		memcpy(s->col, &b[ZONE_RGB1], 3);
		memcpy(s->out, s->col, 3);
		break;
	case 1:
		if (b[ZONE_SUB] == 1) {
			s->mode = Z_XFADE;
			memcpy(s->col, &b[ZONE_RGB1], 3);
			memcpy(s->tgt, &b[ZONE_RGB2], 3);
		} else {
			s->mode = Z_HUE;
			s->last = 255;
		}
		break;
	case 2:
		s->mode = Z_BREATH;
		memcpy(s->col, &b[ZONE_RGB1], 3);
		s->last = ENV_LAST;
		break;
	case 3:
		s->mode = Z_REACTIVE;
		memcpy(s->col, &b[ZONE_RGB1], 3);
		s->period_ms = (b[ZONE_SPEED] + 32) >> 2;
		s->last = ENV_LAST;
		break;
	default:
		/* effect 4 (custom array) is a no-op in stock: keep the previous state */
		return;
	}
}

void leds_fx_apply_all(void)
{
	for (int z = 0; z < LED_ZONES; z++) {
		leds_fx_apply_zone(z);
	}
}

/* stock xfade_step 0x51048, simplified: one unit per channel per tick, ping-pong */
static void xfade_step(struct zone *s, uint8_t z)
{
	bool done = true;

	for (int c = 0; c < 3; c++) {
		if (s->col[c] != s->tgt[c]) {
			s->col[c] += s->col[c] > s->tgt[c] ? -1 : 1;
			done = false;
		}
	}
	if (done) {
		const uint8_t *b = scfg.zone[z];

		s->flag ^= 1;
		memcpy(s->tgt, s->flag ? &b[ZONE_RGB1] : &b[ZONE_RGB2], 3);
	}
	memcpy(s->out, s->col, 3);
}

static void zone_step(uint8_t z)
{
	struct zone *s = &zs[z];
	const uint8_t *b = scfg.zone[z];
	uint8_t sub = b[ZONE_SUB];

	switch (s->mode) {
	case Z_HUE:
		hsv2rgb(s->idx, 255, 255, s->out);
		s->idx = (s->idx + 1) & 0xFF;
		break;
	case Z_XFADE:
		xfade_step(s, z);
		break;
	case Z_BREATH:
		scale_env(s->col, s->idx, s->out);
		if (s->idx < s->last) {
			s->idx++;
		} else {
			/* stock breathing_end */
			if (sub == 1) {
				s->flag ^= 1;
				memcpy(s->col, s->flag ? &b[ZONE_RGB2] : &b[ZONE_RGB1], 3);
			} else if (sub == 2) {
				s->flag = (s->flag + 1) % 9;
				memcpy(s->col, led_cycle_colours[s->flag], 3);
			}
			s->idx = 0;
		}
		break;
	case Z_REACTIVE:
		if (!s->running) {
			memset(s->out, 0, 3);
			break;
		}
		scale_env(s->col, s->idx, s->out);
		if (s->idx < s->last) {
			s->idx++;
		} else if (sub == 1 && !s->flag) {
			/* second pulse in colour 2, at the slower (speed+16) rate */
			s->flag = 1;
			s->idx = 0;
			memcpy(s->col, &b[ZONE_RGB2], 3);
			s->period_ms = b[ZONE_SPEED] + 16;
		} else {
			s->running = false;
		}
		break;
	default:
		break;
	}
}

void leds_fx_reactive_trigger(void)
{
	for (int z = 0; z < LED_ZONES; z++) {
		struct zone *s = &zs[z];
		const uint8_t *b = scfg.zone[z];

		if (s->mode != Z_REACTIVE) {
			continue;
		}
		s->idx = 0;
		s->flag = 0;
		s->acc_ms = 0;
		s->running = true;
		s->period_ms = (b[ZONE_SPEED] + 32) >> 2;
		if (b[ZONE_SUB] == 1) {
			memcpy(s->col, &b[ZONE_RGB1], 3);
		} else if (b[ZONE_SUB] == 3) {
			if (z == 1 && zs[0].mode == Z_REACTIVE) {
				memcpy(s->col, zs[0].col, 3);	/* wheel copies the logo colour */
			} else {
				/* stock rand_hue: redraw until it differs by > 10 from the old R byte */
				uint8_t h;

				do {
					h = sys_rand32_get();
				} while (ABS((int)h - (int)s->col[0]) <= 10);
				hsv2rgb(h, 255, 255, s->col);
			}
		}
	}
}

/* ---- indications ---- */
void leds_fx_indicate_dpi(uint8_t stage)
{
	if (stage >= DPI_STAGES) {
		return;
	}
	memcpy(dpi_ind.rgb, &scfg.dpi[DPI_RGB(stage)], 3);
	dpi_ind.idx = 0;
	dpi_ind.acc = 0;
	dpi_ind.on = true;
}

void leds_fx_set_lowbat(uint8_t level)
{
	if (level != lowbat) {
		lowbat = level;
		lowbat_phase = true;
		lowbat_acc = 0;
	}
}

void leds_fx_set_charging(bool charging)
{
	if (charging && !chg.on) {
		chg.idx = 65;	/* stock starts at full level */
		chg.acc = 0;
	}
	chg.on = charging;
}

void leds_fx_set_pairing(bool on)
{
	pairing = on;
}

void leds_fx_suspend(bool off)
{
	suspended = off;
	if (off) {
		leds_off();
	}
}

void leds_fx_flash(uint8_t r, uint8_t g, uint8_t b)
{
	flash_rgb[0] = r;
	flash_rgb[1] = g;
	flash_rgb[2] = b;
	flash_start = k_uptime_get();
}

void leds_fx_tick(uint32_t elapsed_ms)
{
	/* advance user effects (always, so they resume in phase) */
	for (int z = 0; z < LED_ZONES; z++) {
		struct zone *s = &zs[z];

		if (s->mode == Z_STATIC) {
			continue;
		}
		s->acc_ms += elapsed_ms;
		while (s->period_ms && s->acc_ms >= s->period_ms) {
			s->acc_ms -= s->period_ms;
			zone_step(z);
		}
	}
	if (dpi_ind.on) {
		dpi_ind.acc += elapsed_ms;
		while (dpi_ind.acc >= DPI_STEP_MS) {
			dpi_ind.acc -= DPI_STEP_MS;
			if (++dpi_ind.idx > ENV_LAST) {
				dpi_ind.on = false;
				break;
			}
		}
	}
	if (chg.on) {
		chg.acc += elapsed_ms;
		while (chg.acc >= CHG_STEP_MS) {
			chg.acc -= CHG_STEP_MS;
			chg.idx = chg.idx >= ENV_LAST ? 0 : chg.idx + 1;
		}
	}
	if (lowbat) {
		lowbat_acc += elapsed_ms;
		int32_t half = lowbat == 1 ? 500 : 1000;

		if (lowbat_acc >= half) {
			lowbat_acc -= half;
			lowbat_phase = !lowbat_phase;
		}
	}

	if (suspended) {
		return;
	}

	/* compose, highest priority first */
	uint8_t rgb[3];
	static const uint8_t black[3];
	static const uint8_t white[3] = {0xFF, 0xFF, 0xFF};
	static const uint8_t red[3] = {0xFF, 0, 0};

	int64_t fl = flash_start < 0 ? -1 : k_uptime_get() - flash_start;

	if (fl >= 0 && fl < 2 * FLASH_BLINKS * FLASH_HALF_MS) {
		uint8_t bri = (fl / FLASH_HALF_MS) % 2 ? 0 : 100;

		set_zone(0, bri, flash_rgb);
		set_zone(1, bri, flash_rgb);
	} else if (dpi_ind.on) {
		scale_env(dpi_ind.rgb, dpi_ind.idx, rgb);
		set_zone(0, DPI_BRI, rgb);
		set_zone(1, DPI_BRI, rgb);
	} else if (pairing) {
		/* not in stock (no LED while pairing): green blink so pairing is visible */
		static const uint8_t green[3] = {0, 0xFF, 0};

		set_zone(0, (k_uptime_get() / 250) & 1 ? 50 : 0, green);
		set_zone(1, (k_uptime_get() / 250) & 1 ? 50 : 0, green);
	} else if (chg.on) {
		scale_env(white, chg.idx, rgb);
		set_zone(0, CHG_BRI, rgb);
		set_zone(1, CHG_BRI, rgb);
	} else if (lowbat) {
		set_zone(0, 0, black);
		set_zone(1, LOWBAT_BRI, lowbat_phase ? red : black);
	} else {
		for (int z = 0; z < LED_ZONES; z++) {
			set_zone(z, zs[z].bri, zs[z].out);
		}
	}
}

void leds_fx_init(void)
{
	leds_fx_apply_all();
}
