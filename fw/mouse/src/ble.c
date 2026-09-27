/*
 * Bluetooth LE HID mouse (HOGP) with battery service, via the NCS HIDS library.
 */
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/settings/settings.h>
#include <bluetooth/services/hids.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "pulsedart.h"
#include "hid_desc.h"

LOG_MODULE_REGISTER(pdble, LOG_LEVEL_INF);

#define REPORT_ID          1
#define REPORT_ID_KBD      2
#define REPORT_ID_CONSUMER 3
#define KBD_LEN            8
#define CONSUMER_LEN       2
#define CONN_LATENCY       30

enum { REP_MOUSE, REP_KBD, REP_CONSUMER };

BT_HIDS_DEF(hids_obj, MOUSE_REPORT_LEN, KBD_LEN, CONSUMER_LEN);

static struct bt_conn *cur_conn;
static volatile bool secured;
static atomic_t in_flight;
static uint32_t last_progress;	/* k_uptime_get_32(): one word, no torn reads */

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, (BT_APPEARANCE_HID_MOUSE & 0xFF),
		      (BT_APPEARANCE_HID_MOUSE >> 8)),
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/*
 * Advertising budget: fast (30-60 ms) for ADV_FAST_MS, then slow (1-1.2 s), then off
 * after ADV_TOTAL_MS until the user touches the mouse again (ble_activity()).
 */
#define ADV_FAST_MS  30000
#define ADV_TOTAL_MS (3 * 60 * 1000)

static volatile bool adv_on;
static void adv_slow_fn(struct k_work *w);
static void adv_stop_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(adv_slow_work, adv_slow_fn);
static K_WORK_DELAYABLE_DEFINE(adv_stop_work, adv_stop_fn);

static int64_t adv_deadline;		/* end of the current advertising budget */
static uint32_t adv_retry_at;		/* after a failed start, retry no earlier */
static bool dropped_by_check;		/* the last disconnect was our link check */

static bool adv_start(const struct bt_le_adv_param *param)
{
	bt_le_adv_stop();
	int err = bt_le_adv_start(param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err && err != -EALREADY) {
		LOG_ERR("adv start %d", err);
		adv_on = false;		/* lets ble_activity() try again */
		adv_retry_at = k_uptime_get_32() + 1000;
		return false;
	}
	adv_on = true;
	return true;
}

/* a new advertising budget: fast, then slow, then off */
static void advertise(void)
{
	adv_deadline = k_uptime_get() + ADV_TOTAL_MS;
	adv_start(BT_LE_ADV_CONN_FAST_1);
	k_work_reschedule(&adv_slow_work, K_MSEC(ADV_FAST_MS));
	k_work_reschedule(&adv_stop_work, K_MSEC(ADV_TOTAL_MS));
}

static void adv_slow_fn(struct k_work *w)
{
	if (adv_on && !cur_conn) {
		adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, BT_GAP_ADV_SLOW_INT_MIN,
					  BT_GAP_ADV_SLOW_INT_MAX, NULL));
	}
}

static void adv_stop_fn(struct k_work *w)
{
	if (!cur_conn) {
		bt_le_adv_stop();
		adv_on = false;
		LOG_INF("advertising stopped (no host)");
	}
}

/*
 * Link check: a host that is really using the mouse encrypts the link and enables
 * notifications of the mouse (or boot mouse) report within a second or two. Any other
 * link (a stranger, a half-open reconnect, a bonded device idling in the background) is
 * dropped after LINK_CHECK_MS so advertising resumes and the real host can connect.
 */
#define LINK_CHECK_MS 15000

static volatile bool mouse_notif;
static void link_check_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(link_check_work, link_check_fn);

static bool host_subscribed(struct bt_conn *conn)
{
	/* the stack's CCC state also covers a bonded host whose subscription was restored */
	const struct bt_gatt_attr *rep =
		&hids_obj.gp.svc.attrs[hids_obj.inp_rep_group.reports[REP_MOUSE].att_ind];
	const struct bt_gatt_attr *boot =
		&hids_obj.gp.svc.attrs[hids_obj.boot_mouse_inp_rep.att_ind];

	return mouse_notif || bt_gatt_is_subscribed(conn, rep, BT_GATT_CCC_NOTIFY) ||
	       bt_gatt_is_subscribed(conn, boot, BT_GATT_CCC_NOTIFY);
}

static void link_check_fn(struct k_work *w)
{
	if (cur_conn && !(secured && host_subscribed(cur_conn))) {
		LOG_WRN("dropping unused link (secured %d, notify %d)", secured, mouse_notif);
		dropped_by_check = true;
		bt_conn_disconnect(cur_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
}

static void mouse_notif_handler(enum bt_hids_notify_evt evt)
{
	mouse_notif = evt == BT_HIDS_CCCD_EVT_NOTIFY_ENABLED;
}

static void adv_cancel(void)
{
	k_work_cancel_delayable(&adv_slow_work);
	k_work_cancel_delayable(&adv_stop_work);
	adv_on = false;	/* a connection ends advertising */
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		advertise();
		return;
	}
	cur_conn = bt_conn_ref(conn);
	adv_cancel();
	mouse_notif = false;
	k_work_reschedule(&link_check_work, K_MSEC(LINK_CHECK_MS));
	secured = false;
	atomic_set(&in_flight, 0);
	bt_hids_connected(&hids_obj, conn);
	bt_conn_set_security(conn, BT_SECURITY_L2);
	/*
	 * 7.5 ms interval for the lowest input lag BLE allows. Peripheral latency lets the
	 * radio skip up to CONN_LATENCY events while there is nothing to send (~0.23 s
	 * while asleep); a report still goes out at the next event, so motion lag is
	 * unchanged. Timeout 4 s > 2 x (1 + latency) x 7.5 ms.
	 */
	static const struct bt_le_conn_param p = BT_LE_CONN_PARAM_INIT(6, 6, CONN_LATENCY, 400);

	bt_conn_le_param_update(conn, &p);
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("BLE connected: %s", addr);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	bt_hids_disconnected(&hids_obj, conn);
	k_work_cancel_delayable(&link_check_work);
	if (cur_conn) {
		bt_conn_unref(cur_conn);
		cur_conn = NULL;
	}
	secured = false;
	LOG_INF("BLE disconnected (0x%02x)", reason);
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
			     uint16_t timeout)
{
	LOG_INF("conn params: interval %u.%02u ms, latency %u, timeout %u ms",
		interval * 125 / 100, (interval * 125) % 100, latency, timeout * 10);
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	secured = !err && level >= BT_SECURITY_L2;
}

static void recycled(void)
{
	if (!dropped_by_check) {
		advertise();	/* a host went away: fresh budget */
		return;
	}
	/*
	 * Our link check dropped a central that does not use the mouse. Keep the running
	 * budget, so a central that keeps reconnecting cannot keep us advertising forever;
	 * the user touching the mouse (ble_activity) starts a new one.
	 */
	dropped_by_check = false;
	int64_t left = adv_deadline - k_uptime_get();

	if (left > 0 && adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, BT_GAP_ADV_SLOW_INT_MIN,
						   BT_GAP_ADV_SLOW_INT_MAX, NULL))) {
		k_work_reschedule(&adv_stop_work, K_MSEC(left));
	} else {
		bt_le_adv_stop();
		adv_on = false;
	}
}

BT_CONN_CB_DEFINE(conn_cbs) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
	.le_param_updated = le_param_updated,
	.recycled = recycled,
};

static void hids_init(void)
{
	static const uint8_t report_map[] = {
		MOUSE_DESC(0x85, REPORT_ID,),
		/* keyboard, as the stock USB interface 2 */
		0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, REPORT_ID_KBD,
		0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,
		0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00,
		0x26, 0xfb, 0x00, 0x05, 0x07, 0x19, 0x00, 0x2a, 0xfb, 0x00, 0x81, 0x00, 0xc0,
		/* consumer, as the stock USB interface 3 */
		0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, REPORT_ID_CONSUMER,
		0x95, 0x01, 0x75, 0x10, 0x15, 0x01, 0x26, 0xff, 0x02, 0x19, 0x01, 0x2a, 0xff, 0x02,
		0x81, 0x00, 0xc0,
	};
	struct bt_hids_init_param p = {0};

	p.rep_map.data = report_map;
	p.rep_map.size = sizeof(report_map);
	p.info.bcd_hid = 0x0101;
	p.info.flags = BT_HIDS_REMOTE_WAKE | BT_HIDS_NORMALLY_CONNECTABLE;
	p.inp_rep_group_init.reports[REP_MOUSE].size = MOUSE_REPORT_LEN;
	p.inp_rep_group_init.reports[REP_MOUSE].id = REPORT_ID;
	p.inp_rep_group_init.reports[REP_MOUSE].handler = mouse_notif_handler;
	p.inp_rep_group_init.reports[REP_KBD].size = KBD_LEN;
	p.inp_rep_group_init.reports[REP_KBD].id = REPORT_ID_KBD;
	p.inp_rep_group_init.reports[REP_CONSUMER].size = CONSUMER_LEN;
	p.inp_rep_group_init.reports[REP_CONSUMER].id = REPORT_ID_CONSUMER;
	p.inp_rep_group_init.cnt = 3;
	p.is_mouse = true;
	p.boot_mouse_notif_handler = mouse_notif_handler;
	int err = bt_hids_init(&hids_obj, &p);

	if (err) {
		LOG_ERR("bt_hids_init %d", err);
	}
}

int ble_start(void)
{
	/*
	 * Register HIDS before bt_enable(): with BT_SETTINGS, Zephyr rejects service
	 * registration after GATT init and before settings are loaded (gatt.c,
	 * "Can't register service after init and before settings are loaded").
	 * Registering first also keeps the DB hash stable across boots.
	 */
	hids_init();

	int err = bt_enable(NULL);

	if (err) {
		return err;
	}
	settings_load_subtree("bt");
	advertise();
	return 0;
}

bool ble_ready(void)
{
	return cur_conn && secured;
}

static void sent_cb(struct bt_conn *conn, void *user_data)
{
	if (atomic_dec(&in_flight) <= 0) {
		atomic_set(&in_flight, 0);
	}
	last_progress = k_uptime_get_32();
}

static int send_rep(uint8_t idx, const uint8_t *buf, uint8_t len)
{
	if (!ble_ready()) {
		return -EAGAIN;
	}
	/* keep at most two notifications queued; the caller accumulates otherwise */
	if (atomic_get(&in_flight) >= 2) {
		/* at 7.5 ms intervals, 100 ms without a completion means a lost callback */
		if (k_uptime_get_32() - last_progress < 100) {
			return -EBUSY;
		}
		atomic_set(&in_flight, 0);
	}
	if (atomic_get(&in_flight) == 0) {
		last_progress = k_uptime_get_32();
	}
	/* hold our own reference: disconnected() may drop cur_conn meanwhile */
	k_sched_lock();
	struct bt_conn *c = cur_conn ? bt_conn_ref(cur_conn) : NULL;

	k_sched_unlock();
	if (!c) {
		return -EAGAIN;
	}
	atomic_inc(&in_flight);
	int err = bt_hids_inp_rep_send(&hids_obj, c, idx, buf, len, sent_cb);

	if (err) {
		atomic_dec(&in_flight);
	}
	bt_conn_unref(c);
	return err;
}

int ble_send(const struct mouse_report *r)
{
	uint8_t buf[MOUSE_REPORT_LEN];

	mouse_report_pack(buf, r);
	return send_rep(REP_MOUSE, buf, sizeof(buf));
}

int ble_send_kbd(uint8_t mod, const uint8_t keys[6])
{
	uint8_t buf[KBD_LEN] = {mod, 0};

	memcpy(&buf[2], keys, 6);
	return send_rep(REP_KBD, buf, sizeof(buf));
}

int ble_send_consumer(uint16_t usage)
{
	uint8_t buf[CONSUMER_LEN] = {usage & 0xFF, usage >> 8};

	return send_rep(REP_CONSUMER, buf, sizeof(buf));
}

void ble_unpair_all(void)
{
	if (cur_conn) {
		bt_conn_disconnect(cur_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
	bt_unpair(BT_ID_DEFAULT, NULL);
	advertise();
}

/* user touched the mouse: advertise again if the budget ran out without a host */
void ble_activity(void)
{
	if (!cur_conn && !adv_on && (int32_t)(k_uptime_get_32() - adv_retry_at) >= 0) {
		advertise();
	}
}

void ble_set_battery(uint8_t soc)
{
	if (soc <= 100) {
		bt_bas_set_battery_level(soc);
	}
}
