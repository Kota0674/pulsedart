/*
 * Wired mode: four HID interfaces like the stock firmware (re/vendor_protocol.md 10):
 * mouse (6-byte report, boot capable), vendor 0xFF13 64-byte IN/OUT (NGENUITY),
 * keyboard (8-byte), consumer (16-bit). With CONFIG_PULSEDART_STOCK_USB_ID the device
 * presents the stock identity 0951:16E2 so HyperX NGENUITY recognises it.
 * The stack is enabled when VBUS appears, so the same image works on battery.
 */
#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#if CONFIG_USBD_DFU
#include <zephyr/usb/class/usbd_dfu.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/sys/reboot.h>
#endif
#include <zephyr/logging/log.h>
#include <string.h>

#include "pulsedart.h"
#include "hid_desc.h"
#include "vendor.h"

LOG_MODULE_REGISTER(pdusb, LOG_LEVEL_INF);

#if CONFIG_PULSEDART_STOCK_USB_ID
#define USB_VID 0x0951
#define USB_PID 0x16E2
#define USB_MFR "Kingston"
#define USB_PRODUCT "HyperX Pulsefire Dart"
#else
#define USB_VID CONFIG_PULSEDART_USB_VID
#define USB_PID CONFIG_PULSEDART_USB_PID
#define USB_MFR "Pulsedart project"
#define USB_PRODUCT "Pulsedart mouse"
#endif

USBD_DEVICE_DEFINE(pd_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), USB_VID, USB_PID);
USBD_DESC_LANG_DEFINE(pd_lang);
USBD_DESC_MANUFACTURER_DEFINE(pd_mfr, USB_MFR);
USBD_DESC_PRODUCT_DEFINE(pd_product, USB_PRODUCT);
USBD_DESC_STRING_DEFINE(pd_sn, "000000000000", USBD_DUT_STRING_SERIAL_NUMBER);	/* stock */
USBD_CONFIGURATION_DEFINE(pd_config, USB_SCD_SELF_POWERED | USB_SCD_REMOTE_WAKEUP, 250, NULL);

#if CONFIG_USBD_DFU
/*
 * Firmware update (MCUboot build variant): a DFU run-time interface sits next to the HID interfaces. When
 * dfu-util asks for DFU mode, the device re-enumerates as a plain DFU device that
 * writes the signed image to slot 1; MCUboot installs it on the next boot.
 */
USBD_DEVICE_DEFINE(pd_dfu_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CONFIG_PULSEDART_USB_VID, CONFIG_PULSEDART_DFU_PID);
USBD_DESC_LANG_DEFINE(dfu_lang);
USBD_DESC_PRODUCT_DEFINE(dfu_product, "Pulsedart DFU");
USBD_CONFIGURATION_DEFINE(dfu_config, USB_SCD_SELF_POWERED, 250, NULL);

static void dfu_reboot_fn(struct k_work *w)
{
	sys_reboot(SYS_REBOOT_COLD);
}
static K_WORK_DELAYABLE_DEFINE(dfu_reboot_work, dfu_reboot_fn);
#endif

/* stock report descriptors (flash 0x646f8, 0x64744, 0x6476c, 0x647a8) */
static const uint8_t mouse_desc[] = {MOUSE_DESC()};
static const uint8_t vendor_desc[] = {
	0x06, 0x13, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08,
	0x95, 0x40, 0x09, 0x02, 0x81, 0x02, 0x09, 0x03, 0x91, 0x02, 0xc0,
};
static const uint8_t kbd_desc[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00,
	0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
	0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xfb, 0x00, 0x05, 0x07, 0x19, 0x00, 0x2a,
	0xfb, 0x00, 0x81, 0x00, 0xc0,
};
static const uint8_t consumer_desc[] = {
	0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x95, 0x01, 0x75, 0x10, 0x15, 0x01, 0x26, 0xff,
	0x02, 0x19, 0x01, 0x2a, 0xff, 0x02, 0x81, 0x00, 0xc0,
};

enum { IF_MOUSE, IF_VENDOR, IF_KBD, IF_CONSUMER, IF_COUNT };

struct hid_if {
	const struct device *dev;
	volatile bool ready;
	atomic_t busy;
	/* referenced by the USB stack until input_report_done */
	uint8_t buf[VENDOR_LEN] __aligned(4);
};

static struct hid_if ifs[IF_COUNT] = {
	[IF_MOUSE] = {.dev = DEVICE_DT_GET(DT_NODELABEL(hid_mouse))},
	[IF_VENDOR] = {.dev = DEVICE_DT_GET(DT_NODELABEL(hid_vendor))},
	[IF_KBD] = {.dev = DEVICE_DT_GET(DT_NODELABEL(hid_kbd))},
	[IF_CONSUMER] = {.dev = DEVICE_DT_GET(DT_NODELABEL(hid_consumer))},
};

static bool usb_enabled;
static int64_t next_wakeup_ms;
static bool mouse_boot_proto;

/* vendor requests are handled in the main loop, not in USB context */
K_MSGQ_DEFINE(vendor_q, VENDOR_LEN, 4, 4);

static struct hid_if *if_of(const struct device *dev)
{
	for (int i = 0; i < IF_COUNT; i++) {
		if (ifs[i].dev == dev) {
			return &ifs[i];
		}
	}
	return &ifs[IF_MOUSE];
}

static void iface_ready(const struct device *dev, const bool ready)
{
	struct hid_if *h = if_of(dev);

	h->ready = ready;
	if (!ready) {
		atomic_clear(&h->busy);
	}
	if (h == &ifs[IF_MOUSE]) {
		mouse_boot_proto = false;	/* class re-enable = Report protocol, no callback */
	}
}

static void input_report_done(const struct device *dev, const uint8_t *const report)
{
	atomic_clear(&if_of(dev)->busy);
}

static int get_report(const struct device *dev, const uint8_t type, const uint8_t id,
		      const uint16_t len, uint8_t *const buf)
{
	uint16_t n = dev == ifs[IF_VENDOR].dev	 ? VENDOR_LEN
		     : dev == ifs[IF_KBD].dev	 ? 8
		     : dev == ifs[IF_CONSUMER].dev ? 2
						   : MOUSE_REPORT_LEN;

	if (type != HID_REPORT_TYPE_INPUT || len < n) {
		return -ENOTSUP;
	}
	memset(buf, 0, n);
	return n;
}

static void vendor_in(const uint8_t *buf, uint16_t len)
{
	uint8_t m[VENDOR_LEN] = {0};

	memcpy(m, buf, MIN(len, VENDOR_LEN));
	if (k_msgq_put(&vendor_q, m, K_NO_WAIT)) {
		LOG_WRN("vendor request dropped");
	}
}

static void output_report(const struct device *dev, const uint16_t len,
			  const uint8_t *const buf)
{
	if (dev == ifs[IF_VENDOR].dev) {
		vendor_in(buf, len);
	}
}

/* stock also accepts the vendor command as SET_REPORT(Output) on EP0 */
static int set_report(const struct device *dev, const uint8_t type, const uint8_t id,
		      const uint16_t len, const uint8_t *const buf)
{
	if (dev == ifs[IF_VENDOR].dev && type == HID_REPORT_TYPE_OUTPUT) {
		vendor_in(buf, len);
		return 0;
	}
	return -ENOTSUP;
}

static void set_protocol(const struct device *dev, const uint8_t proto)
{
	if (dev == ifs[IF_MOUSE].dev) {
		mouse_boot_proto = proto == 0;	/* HID boot protocol */
	}
}

static const struct hid_device_ops hid_ops = {
	.iface_ready = iface_ready,
	.get_report = get_report,
	.set_report = set_report,
	.set_protocol = set_protocol,
	.input_report_done = input_report_done,
	.output_report = output_report,
};

static void msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg);

#if CONFIG_USBD_DFU
static void switch_to_dfu_mode(struct usbd_context *const ctx)
{
	int err;

	LOG_INF("switching to DFU mode");
	usbd_disable(ctx);
	usbd_shutdown(ctx);

	if ((err = usbd_add_descriptor(&pd_dfu_usbd, &dfu_lang)) ||
	    (err = usbd_add_descriptor(&pd_dfu_usbd, &dfu_product)) ||
	    (err = usbd_add_configuration(&pd_dfu_usbd, USBD_SPEED_FS, &dfu_config)) ||
	    (err = usbd_register_class(&pd_dfu_usbd, "dfu_dfu", USBD_SPEED_FS, 1))) {
		LOG_ERR("DFU mode setup %d", err);
		return;
	}
	usbd_device_set_code_triple(&pd_dfu_usbd, USBD_SPEED_FS, 0, 0, 0);
	if ((err = usbd_init(&pd_dfu_usbd)) ||
	    (err = usbd_msg_register_cb(&pd_dfu_usbd, msg_cb)) ||
	    (err = usbd_enable(&pd_dfu_usbd))) {
		LOG_ERR("DFU mode enable %d", err);
	}
}
#endif

static void msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
	switch (msg->type) {
#if CONFIG_USBD_DFU
	case USBD_MSG_DFU_APP_DETACH:
		switch_to_dfu_mode(ctx);
		break;
	case USBD_MSG_DFU_DOWNLOAD_COMPLETED:
		/* MCUboot checks the signature and installs slot 1 on the next boot */
		boot_request_upgrade(BOOT_UPGRADE_PERMANENT);
		LOG_INF("update received, rebooting into MCUboot");
		k_work_schedule(&dfu_reboot_work, K_MSEC(500));
		break;
#endif
	case USBD_MSG_VBUS_READY:
		if (!usb_enabled && !usbd_enable(ctx)) {
			usb_enabled = true;
		}
		break;
	case USBD_MSG_VBUS_REMOVED:
		if (usb_enabled && !usbd_disable(ctx)) {
			usb_enabled = false;
		}
		for (int i = 0; i < IF_COUNT; i++) {
			ifs[i].ready = false;
			atomic_clear(&ifs[i].busy);
		}
		break;
	case USBD_MSG_SUSPEND:
		next_wakeup_ms = 0;
		break;
	default:
		break;
	}
}

/* the DFU-mode class belongs to the separate DFU device context only */
static const char *const dfu_blocklist[] = {"dfu_dfu", NULL};

int usb_init(void)
{
	static const struct {
		const uint8_t *d;
		size_t n;
	} descs[IF_COUNT] = {
		{mouse_desc, sizeof(mouse_desc)},
		{vendor_desc, sizeof(vendor_desc)},
		{kbd_desc, sizeof(kbd_desc)},
		{consumer_desc, sizeof(consumer_desc)},
	};
	int err;

	for (int i = 0; i < IF_COUNT; i++) {
		err = hid_device_register(ifs[i].dev, descs[i].d, descs[i].n, &hid_ops);
		if (err) {
			return err;
		}
	}
	if ((err = usbd_add_descriptor(&pd_usbd, &pd_lang)) ||
	    (err = usbd_add_descriptor(&pd_usbd, &pd_mfr)) ||
	    (err = usbd_add_descriptor(&pd_usbd, &pd_product)) ||
	    (err = usbd_add_descriptor(&pd_usbd, &pd_sn)) ||
	    (err = usbd_add_configuration(&pd_usbd, USBD_SPEED_FS, &pd_config)) ||
	    (err = usbd_register_all_classes(&pd_usbd, USBD_SPEED_FS, 1, dfu_blocklist))) {
		return err;
	}
#if CONFIG_USBD_CDC_ACM_CLASS
	/* debug build: a CDC ACM log port needs the IAD device class */
	usbd_device_set_code_triple(&pd_usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 0x02, 0x01);
#else
	usbd_device_set_code_triple(&pd_usbd, USBD_SPEED_FS, 0, 0, 0);
#endif
	usbd_device_set_bcd_device(&pd_usbd, 0x1108);	/* stock FW 1.1.0.8 */
	usbd_msg_register_cb(&pd_usbd, msg_cb);

	err = usbd_init(&pd_usbd);
	if (err) {
		return err;
	}
	if (!usbd_can_detect_vbus(&pd_usbd)) {
		err = usbd_enable(&pd_usbd);
		usb_enabled = !err;
	}
	return err;
}

bool usb_suspended(void)
{
	return usb_enabled && usbd_is_suspended(&pd_usbd);
}

bool usb_ready(void)
{
	return ifs[IF_MOUSE].ready;
}

/* -EAGAIN: no link (caller drops), -EBUSY: previous report still queued (caller retries) */
static int send(int i, const uint8_t *data, uint16_t len)
{
	struct hid_if *h = &ifs[i];
	int err;

	if (!h->ready) {
		return -EAGAIN;
	}
	if (usbd_is_suspended(&pd_usbd)) {
		int64_t now = k_uptime_get();

		if (i != IF_VENDOR && now >= next_wakeup_ms) {
			err = usbd_wakeup_request(&pd_usbd);
			next_wakeup_ms = (err == -EACCES) ? INT64_MAX : now + 500;
		}
		return -EAGAIN;
	}
	if (atomic_set(&h->busy, 1)) {
		return -EBUSY;
	}
	memcpy(h->buf, data, len);
	err = hid_device_submit_report(h->dev, len, h->buf);
	if (err) {
		atomic_clear(&h->busy);
	}
	return err;
}

int usb_send(const struct mouse_report *r)
{
	uint8_t b[MOUSE_REPORT_LEN];

	if (mouse_boot_proto) {
		/* boot protocol: 3 bytes, buttons 0..2, X/Y clamped to int8 (stock) */
		b[0] = r->buttons & 0x07;
		b[1] = (uint8_t)CLAMP(r->dx, -127, 127);
		b[2] = (uint8_t)CLAMP(r->dy, -127, 127);
		return send(IF_MOUSE, b, 3);
	}
	mouse_report_pack(b, r);
	return send(IF_MOUSE, b, sizeof(b));
}

int usb_send_kbd(uint8_t mod, const uint8_t keys[6])
{
	uint8_t b[8] = {mod, 0};

	if (mouse_boot_proto) {
		return 0;	/* stock sends nothing extra in boot protocol */
	}
	memcpy(&b[2], keys, 6);
	return send(IF_KBD, b, sizeof(b));
}

int usb_send_consumer(uint16_t usage)
{
	uint8_t b[2] = {usage & 0xFF, usage >> 8};

	if (mouse_boot_proto) {
		return 0;
	}
	return send(IF_CONSUMER, b, sizeof(b));
}

/* async status (stock 0x5af0c): 64-byte vendor IN report on IF1, 8 status bytes + zeros */
int usb_send_status(const uint8_t st[8])
{
	uint8_t b[VENDOR_LEN] = {0};

	if (mouse_boot_proto) {
		return 0;	/* stock 0x5af0c: no status in boot protocol */
	}
	memcpy(b, st, 8);
	return send(IF_VENDOR, b, sizeof(b));
}

void usb_poll(void)
{
	static uint8_t reply[VENDOR_LEN];
	static uint8_t req[VENDOR_LEN];
	static bool reply_pending;

	if (!reply_pending && !k_msgq_get(&vendor_q, req, K_NO_WAIT)) {
		reply_pending = vendor_handle(req, reply, true);
	}
	if (reply_pending) {
		if (send(IF_VENDOR, reply, VENDOR_LEN) != -EBUSY) {
			reply_pending = false;
		}
		return;
	}
	/* stock streams custom LED arrays chunk by chunk after each IN completes */
	if (!atomic_get(&ifs[IF_VENDOR].busy) && vendor_stream_next(reply)) {
		reply_pending = true;
	}
}
