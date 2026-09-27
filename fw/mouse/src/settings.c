/*
 * Persistent settings in our own storage partition (0xF0000). The stock pairing
 * page (0xEF000) and the external EEPROM are only ever read, never written.
 */
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdlib.h>

#include "pulsedart.h"

LOG_MODULE_REGISTER(pdset, LOG_LEVEL_INF);

#define STOCK_PAIRING_OFFSET 0x1000	/* 0xEF000 inside stock-data (0xEE000) */

static struct pd_settings cfg = {.mode = MODE_ESB};

static int set_cb(const char *key, size_t len, settings_read_cb read_cb, void *arg)
{
	if (!strcmp(key, "mode") && len == 1) {
		read_cb(arg, &cfg.mode, 1);
	} else if (!strcmp(key, "dpi") && len == 1) {
		read_cb(arg, &cfg.dpi_idx, 1);
	} else if (!strcmp(key, "pair") && len == 1) {
		read_cb(arg, &cfg.pair_on_boot, 1);
	} else if (!strcmp(key, "esb") && len == sizeof(cfg.esb_record)) {
		read_cb(arg, cfg.esb_record, sizeof(cfg.esb_record));
		cfg.esb_record_valid = cfg.esb_record[6] == 1;
		cfg.esb_record_dirty = cfg.esb_record_valid;	/* stored = from our pairing */
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(pd, "pd", NULL, set_cb, NULL, NULL);

/* Fall back to the pairing record the stock firmware left at 0xEF000 */
static void load_stock_record(void)
{
	const struct flash_area *fa;
	uint8_t rec[8];

	if (flash_area_open(PARTITION_ID(stock_data_partition), &fa)) {
		return;
	}
	if (!flash_area_read(fa, STOCK_PAIRING_OFFSET, rec, sizeof(rec)) && rec[6] == 1) {
		memcpy(cfg.esb_record, rec, sizeof(rec));
		cfg.esb_record_valid = true;
		LOG_INF("using stock pairing record: ch %u addr %02x %02x %02x %02x",
			rec[0], rec[2], rec[3], rec[4], rec[5]);
	}
	flash_area_close(fa);
}

/* Build-time default record (CONFIG_PULSEDART_ESB_RECORD) */
static bool load_config_record(void)
{
	const char *str = CONFIG_PULSEDART_ESB_RECORD;
	uint8_t rec[8];
	size_t n = 0;

	while (*str && n < sizeof(rec)) {
		char *end;
		unsigned long v = strtoul(str, &end, 16);

		if (end == str || v > 0xFF) {
			return false;
		}
		rec[n++] = v;
		str = end;
	}
	if (n != sizeof(rec) || rec[6] != 1) {
		return false;
	}
	memcpy(cfg.esb_record, rec, sizeof(rec));
	cfg.esb_record_valid = true;
	LOG_INF("using built-in pairing record: ch %u addr %02x %02x %02x %02x",
		rec[0], rec[2], rec[3], rec[4], rec[5]);
	return true;
}

int pd_settings_init(void)
{
	int err = settings_subsys_init();

	if (!err) {
		err = settings_load_subtree("pd");
	}
	if (!cfg.esb_record_valid && !load_config_record()) {
		load_stock_record();
	}
	if (cfg.mode > MODE_BLE) {
		cfg.mode = MODE_ESB;
	}
	return err;
}

struct pd_settings *pd_settings_get(void)
{
	return &cfg;
}

void pd_settings_save(void)
{
	settings_save_one("pd/mode", &cfg.mode, 1);
	settings_save_one("pd/dpi", &cfg.dpi_idx, 1);
	settings_save_one("pd/pair", &cfg.pair_on_boot, 1);
	/* only records from our own pairing; built-in/stock ones keep their precedence */
	if (cfg.esb_record_valid && cfg.esb_record_dirty) {
		settings_save_one("pd/esb", cfg.esb_record, sizeof(cfg.esb_record));
	}
}
