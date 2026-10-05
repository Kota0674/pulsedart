/*
 * Data taken from the stock firmware 1.1.0.8: the PMW3389 SROM (re/sensor.md section 4) and
 * the LED tables (re/led_effects.md). The arrays live in their own file so that a built
 * image holds each of them as one contiguous block:
 *  - stock_blobs.c             real data from your own dump (tools/extract_blobs.py), not in git
 *  - stock_blobs_placeholder.c marker bytes for release builds; tools/make_firmware.py swaps
 *                              in the real data (CONFIG_PULSEDART_BLOB_PLACEHOLDER)
 */
#pragma once
#include <stdint.h>

#define PMW3389_SROM_ID  0x05
#define PMW3389_SROM_LEN 4094

extern const uint8_t pmw3389_srom[PMW3389_SROM_LEN];
extern const uint8_t led_gamma[256];
extern const uint8_t led_breath[188];
extern const uint8_t led_cycle_colours[9][3];
