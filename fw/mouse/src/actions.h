/*
 * Button actions (stock button map, 8 entries x {type, code, aux}) and macro player.
 * Spec: re/actions_macros.md. Stock bugs listed there are fixed, not copied.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Physical map entries, stock order */
enum map_entry {
	MAP_LEFT = 0,
	MAP_RIGHT = 1,
	MAP_MIDDLE = 2,
	MAP_FORWARD = 3,	/* P1.04 */
	MAP_BACK = 4,		/* P1.02 */
	MAP_DPI = 5,
	MAP_WHEEL_UP = 6,
	MAP_WHEEL_DOWN = 7,
};

/* What the host should currently see, built from button actions + macros */
struct host_state {
	uint8_t mbtn;		/* HID mouse buttons */
	int8_t wheel;		/* accumulated wheel steps to send */
	uint8_t kmod;		/* keyboard modifier bitmap */
	uint8_t keys[6];	/* keyboard key usages */
	uint16_t consumer;	/* consumer usage, 0 = none */
};

void actions_init(void);
/*
 * Call every tick with the debounced physical state of map entries 0..5 (bit n =
 * entry n) and the wheel detents since the last call (+ = up). Updates *hs.
 */
void actions_tick(uint8_t phys, int8_t wheel, uint32_t elapsed_ms, struct host_state *hs);
/* Suppress map actions of these physical entries (combo keys) */
void actions_set_suppressed(uint8_t mask);
bool actions_busy(void);	/* macro playing: keep the mouse awake */
/* map/macros replaced (factory reset, DF): stop the player, drop the sniper */
void actions_reset(void);
/* after the report step: which host views equal the current state (macro handshake) */
void actions_host_synced(bool mouse, bool kbd);

/* DPI helpers shared with the vendor protocol */
void dpi_apply(void);			/* write current stage (or sniper) to the sensor */
void dpi_step(uint8_t mode);		/* 0 up, 1 down, 2 up-wrap, 3 down-wrap */
void dpi_select(uint8_t stage);
uint8_t dpi_current(void);
bool dpi_sniper_active(void);
