/*
 * Settings model identical to the stock firmware 1.1.0.8, stored in the same external
 * I2C EEPROM (0x50) at the same addresses, so NGENUITY profiles and the user's
 * existing settings carry over. Layout: re/input_usb_led.md section 5, re/eeprom.md.
 *
 * Blocks are kept as raw byte arrays with named offsets, exactly as on the EEPROM.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ---- cfg, EEPROM 0x0290, 27 bytes ---- */
#define CFG_LEN              27
#define CFG_POLL_IDX         0x0E	/* 0..3 -> 125/250/500/1000 Hz */
#define CFG_DEBOUNCE_PRESS   0x0F
#define CFG_DEBOUNCE_RELEASE 0x10
#define CFG_IDLE_T1          0x11	/* u32 LE, ms */
#define CFG_IDLE_T2          0x15	/* u32 LE, ms */
#define CFG_CUSTOM_CNT(z)    (0x19 + (z))	/* custom LED entries * 3, per zone */

/* ---- misc, EEPROM 0x0060, 9 bytes ---- */
#define MISC_LEN             9
#define MISC_SOC             0
#define MISC_POWER_STATE     1
#define MISC_LOWBAT_PCT      2
#define MISC_MV              6	/* u16 LE */
#define MISC_GAUGE_STATUS    8

/* ---- DPI, EEPROM 0x0090, 38 bytes ---- */
#define DPI_LEN              0x26
#define DPI_CUR              0x00
#define DPI_COUNT            0x02
#define DPI_MIN              0x03	/* u16 LE, 50 CPI units */
#define DPI_MAX              0x05
#define DPI_MASK             0x07
#define DPI_SNIPER           0x08	/* u16 LE */
#define DPI_VAL(i)           (0x0A + 2 * (i))	/* u16 LE, 5 stages */
#define DPI_RGB(i)           (0x17 + 3 * (i))
#define DPI_STAGES           5

/* ---- LED zones, EEPROM 0x0110, 2 x 13 bytes ---- */
#define ZONE_LEN             13
#define ZONE_EFFECT          0
#define ZONE_SUB             1
#define ZONE_SPEED           2
#define ZONE_BRIGHTNESS      6
#define ZONE_RGB1            7
#define ZONE_RGB2            10
#define LED_ZONES            2

/* ---- button map, EEPROM 0x0190, 8 x 3 bytes (type, code, aux) ---- */
#define BTN_MAP_ENTRIES      8	/* 0 L, 1 R, 2 M, 3 fwd (P1.04), 4 back (P1.02), 5 DPI, 6 wheel up, 7 wheel down */
#define BTN_MAP_LEN          (BTN_MAP_ENTRIES * 3)

/* ---- macros ---- */
#define MACROS               6
#define MACRO_HDR_LEN        9
#define MACRO_DATA_LEN       0x200
/* macro 5 data would run into the stock pairing history at EEPROM 0x1488 */
#define MACRO_DATA_CAP(i)    ((i) == MACROS - 1 ? 0x1F8 : MACRO_DATA_LEN)

/* ---- custom LED arrays: 48 RGB entries per zone (0x70 + 0x20 bytes on EEPROM) ---- */
#define CUSTOM_LED_ENTRIES   48
#define CUSTOM_LED_LEN       (CUSTOM_LED_ENTRIES * 3)

struct stockcfg {
	uint8_t cfg[CFG_LEN];
	uint8_t misc[MISC_LEN];
	uint8_t dpi[DPI_LEN];
	uint8_t zone[LED_ZONES][ZONE_LEN];
	uint8_t btn[BTN_MAP_LEN];
	uint8_t macro_hdr[MACROS][MACRO_HDR_LEN];
	uint8_t macro_data[MACROS][MACRO_DATA_LEN];
	uint8_t custom_led[LED_ZONES][CUSTOM_LED_LEN];
};

/* Save/defaults block numbering used by vendor commands DE / DF */
enum stockcfg_block {
	BLK_CFG = 0,		/* cfg + custom LED arrays */
	BLK_MISC = 1,
	BLK_LED = 2,
	BLK_DPI = 3,
	BLK_BUTTONS = 4,
	BLK_MACROS = 5,
	BLK_ALL = 0xFF,
};

extern struct stockcfg scfg;

/* Load from EEPROM; falls back to stock defaults (RAM only) if the layout differs. */
int stockcfg_init(void);
/* Queue a background write of a block (or BLK_ALL) to the EEPROM. */
void stockcfg_save(enum stockcfg_block blk);
/* Reset a block (or BLK_ALL) in RAM to the stock factory defaults. */
void stockcfg_defaults(enum stockcfg_block blk);
/* Wait for queued EEPROM writes (before reset, DFU or System OFF) */
void stockcfg_flush(void);

static inline uint16_t sc_get16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static inline uint32_t sc_get32(const uint8_t *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void sc_put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

/* stock pairing history, cached at boot (never written by us) */
extern uint8_t stockcfg_pair_count;
extern uint8_t stockcfg_pair_hist[8][8];

/* Raw EEPROM access (16-bit address, 128-byte pages), serialized internally */
int eeprom_read(uint16_t addr, void *buf, size_t len);
int eeprom_write(uint16_t addr, const void *buf, size_t len);
