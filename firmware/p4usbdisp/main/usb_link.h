#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/*
 * Wire protocol, host -> device, on the bulk OUT endpoint of interface 0.
 *
 * Each message is two bulk writes: a 16-byte header (a short packet, which ends
 * the device-side transfer), then exactly `len` payload bytes, which the device
 * collects into a PSRAM frame slot (see usb_link.c for why via bounce buffers).
 *
 * Resync: the host issues a USB port reset before streaming. That drops any
 * half-received message and re-arms the header read.
 *
 * All fields little-endian.
 */
#define P4D_MAGIC          0x31443450u /* "P4D1" */
#define P4D_TYPE_JPEG_FULL 1           /* baseline JPEG, 4:2:0, 800x1280 portrait */
#define P4D_TYPE_BENCH     2           /* diagnostic: received into a slot like a frame, then dropped */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  type;
    uint8_t  flags;
    uint16_t reserved;
    uint32_t len;
    uint32_t seq;
} p4d_hdr_t;

/* Vendor control requests (recipient interface, wIndex 0). */
#define P4D_REQ_GET_STATS  0x01 /* IN:  usb_link_stats_t + decode stats (see main.c) */
#define P4D_REQ_BACKLIGHT  0x02 /* OUT: wValue = 0..100 */

typedef struct __attribute__((packed)) {
    uint64_t rx_bytes;
    uint32_t frames_rx;   /* complete messages received */
    uint32_t frames_stale;/* received but replaced by a newer one before decode */
    uint32_t bad_header;
    uint32_t truncated;   /* payload ended early (short packet) */
    uint32_t oversize;    /* len > slot size, payload discarded */
} usb_link_stats_t;

/* Max JPEG payload per frame. A desktop frame at q85 is typically 50-250 KB. */
#define USB_LINK_SLOT_BYTES (1024 * 1024)

/* Frame slot handed to the consumer. */
typedef struct {
    int      slot;
    uint8_t *data;
    uint32_t len;
    uint32_t seq;
    uint8_t  type;
} usb_link_frame_t;

esp_err_t usb_link_init(void);

/* Blocks up to `wait` for the newest complete frame. Returns false on timeout.
 * Older frames that arrived meanwhile are dropped (counted as stale). The slot
 * belongs to the caller until usb_link_release(), and payload reception is
 * paused until then (see usb_link.c) - so release as soon as the decode ends. */
bool usb_link_take(usb_link_frame_t *out, TickType_t wait);
void usb_link_release(const usb_link_frame_t *f);

void usb_link_get_stats(usb_link_stats_t *out);

/* Extra bytes appended to the GET_STATS reply; set by the application. */
typedef void (*usb_link_stats_ext_cb_t)(uint8_t *buf, uint16_t *len, uint16_t max);
void usb_link_set_stats_ext(usb_link_stats_ext_cb_t cb);

/* Called from the USB task on P4D_REQ_BACKLIGHT. */
typedef void (*usb_link_backlight_cb_t)(int percent);
void usb_link_set_backlight_cb(usb_link_backlight_cb_t cb);
