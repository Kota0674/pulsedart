/* Mouse report descriptor shared by USB (no report ID) and BLE (report ID 1). */
#pragma once
#include <stdint.h>
#include "pulsedart.h"

#define MOUSE_REPORT_LEN 6	/* buttons, X lo/hi, Y lo/hi, wheel */

#define MOUSE_DESC_BODY                                                         \
	0x05, 0x09,		/*   Usage Page (Buttons) */                    \
	0x19, 0x01,		/*   Usage Minimum (1) */                       \
	0x29, 0x05,		/*   Usage Maximum (5) */                       \
	0x15, 0x00,		/*   Logical Minimum (0) */                     \
	0x25, 0x01,		/*   Logical Maximum (1) */                     \
	0x95, 0x05,		/*   Report Count (5) */                        \
	0x75, 0x01,		/*   Report Size (1) */                         \
	0x81, 0x02,		/*   Input (Data,Var,Abs) */                    \
	0x95, 0x01,		/*   Report Count (1) */                        \
	0x75, 0x03,		/*   Report Size (3) */                         \
	0x81, 0x01,		/*   Input (Const) */                           \
	0x05, 0x01,		/*   Usage Page (Generic Desktop) */            \
	0x09, 0x30,		/*   Usage (X) */                               \
	0x09, 0x31,		/*   Usage (Y) */                               \
	0x16, 0x01, 0x80,	/*   Logical Minimum (-32767) */                \
	0x26, 0xFF, 0x7F,	/*   Logical Maximum (32767) */                 \
	0x75, 0x10,		/*   Report Size (16) */                        \
	0x95, 0x02,		/*   Report Count (2) */                        \
	0x81, 0x06,		/*   Input (Data,Var,Rel) */                    \
	0x09, 0x38,		/*   Usage (Wheel) */                           \
	0x15, 0x81,		/*   Logical Minimum (-127) */                  \
	0x25, 0x7F,		/*   Logical Maximum (127) */                   \
	0x75, 0x08,		/*   Report Size (8) */                         \
	0x95, 0x01,		/*   Report Count (1) */                        \
	0x81, 0x06		/*   Input (Data,Var,Rel) */

#define MOUSE_DESC(...)                                             \
	0x05, 0x01,		/* Usage Page (Generic Desktop) */              \
	0x09, 0x02,		/* Usage (Mouse) */                             \
	0xA1, 0x01,		/* Collection (Application) */                  \
	__VA_ARGS__                                                             \
	0x09, 0x01,		/*   Usage (Pointer) */                         \
	0xA1, 0x00,		/*   Collection (Physical) */                   \
	MOUSE_DESC_BODY,                                                        \
	0xC0,			/*   End Collection */                          \
	0xC0			/* End Collection */

static inline void mouse_report_pack(uint8_t *out, const struct mouse_report *r)
{
	out[0] = r->buttons & 0x1F;
	out[1] = (uint8_t)r->dx;
	out[2] = (uint8_t)((uint16_t)r->dx >> 8);
	out[3] = (uint8_t)r->dy;
	out[4] = (uint8_t)((uint16_t)r->dy >> 8);
	out[5] = (uint8_t)r->wheel;
}
