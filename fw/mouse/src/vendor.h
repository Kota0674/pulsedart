/*
 * Vendor (NGENUITY) configuration protocol, 64-byte requests/replies.
 * Spec: re/vendor_protocol.md (byte-exact model of stock 1.1.0.8).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define VENDOR_LEN 64

/*
 * Handle one request. usb = true for the USB vendor interface, false for the dongle
 * tunnel. Fills out[64]; returns true if a reply should be sent now.
 */
bool vendor_handle(uint8_t *req, uint8_t *out, bool usb);

/* USB streaming of custom LED arrays (50 01 / 50 02): next chunk after an IN completes */
bool vendor_stream_next(uint8_t *out);

/* Main-loop hook: run actions that must happen outside the request context */
void vendor_poll(void);
