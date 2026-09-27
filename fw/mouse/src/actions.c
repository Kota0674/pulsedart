/*
 * Stock-compatible button actions and macros (re/actions_macros.md), with the stock
 * bugs fixed: full keyboard report (all modifiers, 6 keys), macro event count from the
 * stored counters, delay >= 1 ms, bounded data, macro wheel over every transport,
 * everything a macro pressed is released when it stops.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "pulsedart.h"
#include "stockcfg.h"
#include "actions.h"
#include "leds_fx.h"

LOG_MODULE_REGISTER(actions, LOG_LEVEL_INF);

#define T_MOUSE    1
#define T_KEY      2
#define T_CONSUMER 3
#define T_MACRO    4
#define T_SPECIAL  7

#define SP_DPI_UP        6
#define SP_DPI_DOWN      7
#define SP_DPI_UP_WRAP   8
#define SP_DPI_DOWN_WRAP 9
#define SP_SNIPER        0x0B

#define WHEEL_UP_CODE    0xE8
#define WHEEL_DOWN_CODE  0xE9

#define MACRO_START_DELAY_MS 100	/* stock: first event 100 ms after the press */
#define MP_OUT_MOUSE      1
#define MP_OUT_KBD        2
#define MP_OUT_TIMEOUT_MS 100	/* link down: do not stall a macro forever */
#define MACRO_PASS_GAP_MS    2

/* stock table 0x66953: code 1..5 -> HID bit (4 = button 5, 5 = button 4) */
static const uint8_t mouse_bit[6] = {0, 0x01, 0x02, 0x04, 0x10, 0x08};
/* stock table 0x6694c: consumer code 0..6 */
static const uint16_t consumer_usage[7] = {0xCD, 0xB7, 0xB6, 0xB5, 0xE2, 0xEA, 0xE9};

static uint8_t prev_phys;
static uint8_t suppressed;
/* wheel entries mapped to non-wheel actions: pressed for TAP_MS, then released */
#define TAP_MS 20
static int16_t tap_left_ms[2];

/* per-entry resources, so only the owner releases what it pressed */
static uint8_t own_mbtn[BTN_MAP_ENTRIES];
static uint8_t own_key[BTN_MAP_ENTRIES];
static uint16_t own_consumer[BTN_MAP_ENTRIES];

static uint8_t btn_keys[6];	/* keyboard usages held by buttons (incl. modifiers) */
static int8_t btn_wheel;	/* steps from buttons 0..5 mapped to 0xE8/0xE9 */
static uint8_t consumer_owner = 0xFF;

/* ---------------- DPI ---------------- */
static bool sniper;
static uint8_t sniper_owner = 0xFF;

uint8_t dpi_current(void)
{
	return scfg.dpi[DPI_CUR];
}

bool dpi_sniper_active(void)
{
	return sniper;
}

void dpi_apply(void)
{
	uint16_t v = sniper ? sc_get16(&scfg.dpi[DPI_SNIPER])
			    : sc_get16(&scfg.dpi[DPI_VAL(scfg.dpi[DPI_CUR])]);

	sensor_set_cpi(CLAMP(v, 2, 320) * 50);
}

void dpi_select(uint8_t stage)
{
	if (stage >= DPI_STAGES || !(scfg.dpi[DPI_MASK] & BIT(stage))) {
		return;
	}
	scfg.dpi[DPI_CUR] = stage;
	dpi_apply();
	leds_fx_indicate_dpi(stage);
}

/* stock 0x62e34: step through the enabled stages */
void dpi_step(uint8_t mode)
{
	uint8_t mask = scfg.dpi[DPI_MASK] & 0x1F;
	int cur = scfg.dpi[DPI_CUR];
	bool up = mode == 0 || mode == 2;
	bool wrap = mode >= 2;

	if (!mask) {
		return;
	}
	for (int n = 0; n < DPI_STAGES; n++) {
		cur += up ? 1 : -1;
		if (cur >= DPI_STAGES || cur < 0) {
			if (!wrap) {
				/* stop at the last / first enabled stage; stock still pulses the LEDs */
				leds_fx_indicate_dpi(scfg.dpi[DPI_CUR]);
				return;
			}
			cur = up ? 0 : DPI_STAGES - 1;
		}
		if (mask & BIT(cur)) {
			dpi_select(cur);	/* RAM only; NGENUITY persists it with DE 3 / DE FF */
			return;
		}
	}
}

/* ---------------- keyboard helpers ---------------- */
static void key_add(uint8_t usage)
{
	for (int i = 0; i < 6; i++) {
		if (btn_keys[i] == usage) {
			return;
		}
	}
	for (int i = 0; i < 6; i++) {
		if (!btn_keys[i]) {
			btn_keys[i] = usage;
			return;
		}
	}
	btn_keys[5] = usage;	/* full: overwrite the last slot, as stock */
}

static void key_remove(uint8_t usage)
{
	for (int i = 0; i < 6; i++) {
		if (btn_keys[i] == usage) {
			memmove(&btn_keys[i], &btn_keys[i + 1], 5 - i);
			btn_keys[5] = 0;
			return;
		}
	}
}

/* ---------------- macro player ---------------- */
static struct {
	bool running;
	uint8_t idx;		/* macro index */
	uint8_t entry;		/* map entry that started it */
	uint8_t mode;
	uint16_t count;		/* events per pass */
	uint16_t played;
	uint16_t off;
	uint16_t passes;
	uint16_t repeat;
	int32_t wait_ms;
	/* current output of the macro */
	uint8_t mbtn;
	int8_t wheel;
	uint8_t kmod;
	uint8_t keys[6];
	uint8_t out_wait;	/* MP_OUT_*: last event's output not yet reported */
	uint16_t out_wait_ms;
} mp;

static uint16_t macro_event_count(uint8_t i)
{
	const uint8_t *h = scfg.macro_hdr[i];

	/* stock uses len's low byte as a count; the recounted n5 + n10 are reliable */
	return sc_get16(&h[2]) + sc_get16(&h[4]);
}

static void macro_stop(void)
{
	mp.running = false;
	mp.out_wait = 0;
	mp.mbtn = 0;
	mp.wheel = 0;
	mp.kmod = 0;
	memset(mp.keys, 0, sizeof(mp.keys));
}

static void macro_start(uint8_t idx, uint8_t entry)
{
	if (idx >= MACROS || macro_event_count(idx) == 0) {
		return;
	}
	const uint8_t *h = scfg.macro_hdr[idx];

	macro_stop();
	mp.running = true;
	mp.idx = idx;
	mp.entry = entry;
	mp.mode = h[6] & 3;
	mp.count = macro_event_count(idx);
	mp.repeat = mp.mode == 1 ? MAX(h[7] | (h[8] << 8), 1) : 1;
	mp.played = 0;
	mp.off = 0;
	mp.passes = 0;
	mp.wait_ms = MACRO_START_DELAY_MS;
}

static void macro_tick(uint32_t elapsed_ms)
{
	if (!mp.running) {
		return;
	}
	mp.wait_ms -= elapsed_ms;
	if (mp.out_wait) {
		mp.out_wait_ms += elapsed_ms;
	}
	while (mp.running && mp.wait_ms <= 0) {
		const uint8_t *d = scfg.macro_data[mp.idx];

		/* stock 0x62980: the next event waits until the previous one was reported */
		if (mp.out_wait && mp.out_wait_ms < MP_OUT_TIMEOUT_MS) {
			mp.wait_ms = 1;
			break;
		}
		mp.out_wait = 0;

		if (mp.played >= mp.count) {
			/* end of pass */
			mp.passes++;
			if ((mp.mode == 1 && mp.passes < mp.repeat) || mp.mode == 2 || mp.mode == 3) {
				mp.played = 0;
				mp.off = 0;
				mp.wait_ms += MACRO_PASS_GAP_MS;
				continue;
			}
			macro_stop();
			return;
		}
		uint16_t delay;

		if (d[mp.off] == 0x1A) {
			if (mp.off + 10 > MACRO_DATA_LEN) {
				macro_stop();
				return;
			}
			mp.kmod = d[mp.off + 1];
			memcpy(mp.keys, &d[mp.off + 2], 6);
			delay = sc_get16(&d[mp.off + 8]);
			mp.off += 10;
			mp.out_wait = MP_OUT_KBD;
		} else {
			if (mp.off + 5 > MACRO_DATA_LEN) {
				macro_stop();
				return;
			}
			mp.mbtn = d[mp.off + 1] & 0x1F;
			mp.wheel += (int8_t)d[mp.off + 2];
			delay = sc_get16(&d[mp.off + 3]);
			mp.off += 5;
			mp.out_wait = MP_OUT_MOUSE;
		}
		mp.out_wait_ms = 0;
		mp.played++;
		mp.wait_ms += MAX(delay, 1);	/* stock reboots on a 0 ms delay */
	}
}

bool actions_busy(void)
{
	return mp.running || sniper;
}

/* ---------------- dispatcher ---------------- */
static void press(uint8_t entry)
{
	const uint8_t *m = &scfg.btn[entry * 3];
	uint8_t type = m[0], code = m[1], aux = m[2];

	switch (type) {
	case T_MOUSE:
		if (code >= 1 && code <= 5) {
			own_mbtn[entry] = mouse_bit[code];
		} else if (code == WHEEL_UP_CODE) {
			btn_wheel++;	/* stock: one scroll step per press */
		} else if (code == WHEEL_DOWN_CODE) {
			btn_wheel--;
		}
		break;
	case T_KEY:
		if (aux) {
			own_key[entry] = aux;
			key_add(aux);
		}
		break;
	case T_CONSUMER:
		if ((code & 0xF) < ARRAY_SIZE(consumer_usage)) {
			own_consumer[entry] = consumer_usage[code & 0xF];
			consumer_owner = entry;	/* stock: last press wins */
		}
		break;
	case T_MACRO:
		/* a press while a toggle macro runs stops it; otherwise (re)start */
		if (mp.running && mp.mode == 2) {
			macro_stop();
		} else {
			macro_start(code, entry);
		}
		break;
	case T_SPECIAL:
		if (code == SP_SNIPER) {
			sniper = true;
			sniper_owner = entry;
			dpi_apply();
		}
		break;
	default:
		break;
	}
}

/* drop what this entry holds; a key shared with another held entry stays */
static void drop_owned(uint8_t entry)
{
	uint8_t k = own_key[entry];

	own_mbtn[entry] = 0;
	own_key[entry] = 0;
	if (k) {
		bool other = false;

		for (int e = 0; e < BTN_MAP_ENTRIES; e++) {
			other |= own_key[e] == k;
		}
		if (!other) {
			key_remove(k);
		}
	}
	own_consumer[entry] = 0;
	if (consumer_owner == entry) {
		consumer_owner = 0xFF;
	}
}

static void release(uint8_t entry)
{
	const uint8_t *m = &scfg.btn[entry * 3];
	uint8_t type = m[0], code = m[1];

	drop_owned(entry);

	/* what this entry started ends with it, whatever the map says now */
	if (mp.running && mp.mode == 3 && mp.entry == entry) {
		macro_stop();
	}
	if (sniper_owner == entry) {
		sniper = false;
		sniper_owner = 0xFF;
		dpi_apply();
		return;
	}
	if (type == T_SPECIAL && code >= SP_DPI_UP && code <= SP_DPI_DOWN_WRAP && !sniper) {
		dpi_step(code - SP_DPI_UP);	/* stock: on release, not while sniper is held */
	}
}

/* a wheel detent is a press + release of entry 6/7 (stock) */
static void wheel_tap(uint8_t entry, struct host_state *hs)
{
	const uint8_t *m = &scfg.btn[entry * 3];

	if (m[0] == T_MOUSE && (m[1] == WHEEL_UP_CODE || m[1] == WHEEL_DOWN_CODE)) {
		int w = hs->wheel + (m[1] == WHEEL_UP_CODE ? 1 : -1);

		hs->wheel = CLAMP(w, -127, 127);
		return;
	}
	/* keys / consumer / buttons on the wheel: hold long enough for every transport */
	int t = entry - MAP_WHEEL_UP;

	if (tap_left_ms[t] > 0) {
		release(entry);
	}
	press(entry);
	tap_left_ms[t] = TAP_MS;
}

/* drop a pressed entry without running its release action (it became a combo key) */
static void cancel(uint8_t entry)
{
	drop_owned(entry);
	if (mp.running && mp.entry == entry && mp.mode == 3) {
		macro_stop();
	}
	if (sniper && sniper_owner == entry) {
		sniper = false;
		sniper_owner = 0xFF;
		dpi_apply();
	}
}

void actions_set_suppressed(uint8_t mask)
{
	suppressed = mask;
}

void actions_reset(void)
{
	macro_stop();
	sniper = false;
	sniper_owner = 0xFF;
	dpi_apply();
}

void actions_host_synced(bool mouse, bool kbd)
{
	if ((mp.out_wait == MP_OUT_MOUSE && mouse) || (mp.out_wait == MP_OUT_KBD && kbd)) {
		mp.out_wait = 0;
	}
}

void actions_tick(uint8_t phys, int8_t wheel, uint32_t elapsed_ms, struct host_state *hs)
{
	uint8_t cancelled = prev_phys & suppressed;

	for (uint8_t e = 0; e < 6; e++) {
		if (cancelled & BIT(e)) {
			cancel(e);
		}
	}
	prev_phys &= ~suppressed;

	uint8_t eff = phys & ~suppressed;
	uint8_t changed = eff ^ prev_phys;

	for (uint8_t e = 0; e < 6; e++) {
		if (changed & BIT(e)) {
			if (eff & BIT(e)) {
				press(e);
				leds_fx_reactive_trigger();	/* stock: every press */
			} else {
				release(e);
			}
		}
	}
	prev_phys = eff;

	for (int t = 0; t < 2; t++) {
		if (tap_left_ms[t] > 0) {
			tap_left_ms[t] -= elapsed_ms;
			if (tap_left_ms[t] <= 0) {
				release(MAP_WHEEL_UP + t);
			}
		}
	}
	for (int i = 0; i < (wheel < 0 ? -wheel : wheel); i++) {
		wheel_tap(wheel > 0 ? MAP_WHEEL_UP : MAP_WHEEL_DOWN, hs);
		leds_fx_reactive_trigger();	/* stock: every wheel detent */
	}

	macro_tick(elapsed_ms);

	/* compose the host view */
	uint8_t mbtn = mp.mbtn;

	for (int e = 0; e < BTN_MAP_ENTRIES; e++) {
		mbtn |= own_mbtn[e];
	}
	hs->mbtn = mbtn;
	hs->consumer = consumer_owner < BTN_MAP_ENTRIES ? own_consumer[consumer_owner] : 0;
	if (btn_wheel) {
		hs->wheel = CLAMP(hs->wheel + btn_wheel, -127, 127);
		btn_wheel = 0;
	}
	if (mp.wheel) {
		hs->wheel = CLAMP(hs->wheel + mp.wheel, -127, 127);
		mp.wheel = 0;
	}

	/* keyboard: modifiers from every E0..E7, up to 6 other keys (stock bug fixed) */
	uint8_t mod = mp.kmod, n = 0;

	memset(hs->keys, 0, sizeof(hs->keys));
	for (int i = 0; i < 6; i++) {
		uint8_t k = btn_keys[i];

		if (k >= 0xE0 && k <= 0xE7) {
			mod |= BIT(k - 0xE0);
		} else if (k && n < 6) {
			hs->keys[n++] = k;
		}
	}
	for (int i = 0; i < 6 && n < 6; i++) {
		if (mp.keys[i]) {
			hs->keys[n++] = mp.keys[i];
		}
	}
	hs->kmod = mod;
}

void actions_init(void)
{
	memset(&mp, 0, sizeof(mp));
	sniper = false;
	dpi_apply();
}
