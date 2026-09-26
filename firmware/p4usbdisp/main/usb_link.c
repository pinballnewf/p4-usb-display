/*
 * USB 2.0 HS link: vendor-class device on the P4's OTG port (middle USB-C).
 *
 * Uses an app-level TinyUSB class driver rather than the stock vendor class:
 * the stock one funnels bulk data through a 512-byte FIFO and topped out at
 * ~7 MB/s, while DMA into large internal-RAM buffers measured ~30 MB/s
 * (firmware/usb_hs_test).
 *
 * Receive state machine, run from TinyUSB's task in the xfer callback:
 *   HEADER  - 512-byte transfer armed; the host's 16-byte header write is a
 *             short packet, which completes it.
 *   PAYLOAD - chunks DMA'd into internal-RAM bounce buffers, then copied into a
 *             PSRAM frame slot until len is in.
 *   DISCARD - same bounce buffers, never copied, for payloads that cannot be taken.
 *
 * Why bounce: DMA straight into PSRAM measured 1.5 MB/s against 27 MB/s into
 * internal RAM. The DWC2 driver invalidates the cache over each received range,
 * and on PSRAM that costs ~11.5 ms per transfer. Copying 32 KB out of internal
 * RAM costs a fraction of that, and overlaps the next transfer because the other
 * bounce buffer is re-armed before the copy starts. The JPEG driver writes the
 * slot back from cache itself before decoding.
 *
 * Three slots, so one can be decoding, one can hold the newest complete frame
 * and one can be filling - a free slot always exists when a header arrives.
 */

#include "usb_link.h"

#include <string.h>

#include "driver/jpeg_decode.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "device/usbd_pvt.h"

#include "audio.h"
#include "touch.h"

static const char *TAG = "usb_link";

/* ---- Descriptors ---- */

#define USB_VID 0x303A
#define USB_PID 0x4020 /* development PID, not allocated */

/* Composite: interface 0 is the display link (vendor class, our own driver),
 * interface 1 a standard HID multi-touch touchscreen (TinyUSB's HID class,
 * bound by the host's hid-multitouch with no host-side code), interfaces 2-3 a
 * UAC2 speaker (TinyUSB's audio class, see audio.c). */
enum { ITF_DISPLAY = 0, ITF_TOUCH, ITF_AUDIO_CTRL, ITF_AUDIO_STREAM, ITF_COUNT };
_Static_assert(ITF_AUDIO_CTRL == AUDIO_ITF_CONTROL && ITF_AUDIO_STREAM == AUDIO_ITF_STREAMING,
               "audio.h interface numbers out of step");
#define EP_OUT   0x01
#define EP_IN    0x81
#define EP_TOUCH 0x82
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_HID_DESC_LEN + AUDIO_SPEAKER_DESC_LEN)

static const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    /* Per-interface classes, and the audio function is grouped by an IAD. */
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0400,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const tusb_desc_device_qualifier_t s_qualifier_desc = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    /* Per-interface classes, and the audio function is grouped by an IAD. */
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 1,
    .bReserved = 0,
};

static const uint8_t s_fs_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_TOTAL_LEN, 0, 500),
    TUD_VENDOR_DESCRIPTOR(ITF_DISPLAY, 4, EP_OUT, EP_IN, 64),
    TUD_HID_DESCRIPTOR(ITF_TOUCH, 5, HID_ITF_PROTOCOL_NONE, TOUCH_HID_REPORT_DESC_LEN, EP_TOUCH, 64, 1),
    AUDIO_SPEAKER_DESCRIPTOR(TUD_AUDIO_EP_SIZE(false, AUDIO_SAMPLE_RATE, AUDIO_BYTES_PER_SMP, AUDIO_CHANNELS), 1),
};

static const uint8_t s_hs_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_TOTAL_LEN, 0, 500),
    TUD_VENDOR_DESCRIPTOR(ITF_DISPLAY, 4, EP_OUT, EP_IN, 512),
    /* bInterval 4 at high speed = 2^(4-1) microframes = 1 ms */
    TUD_HID_DESCRIPTOR(ITF_TOUCH, 5, HID_ITF_PROTOCOL_NONE, TOUCH_HID_REPORT_DESC_LEN, EP_TOUCH, 64, 4),
    /* feedback interval 4 at high speed = every 8 microframes = 1 ms */
    AUDIO_SPEAKER_DESCRIPTOR(TUD_AUDIO_EP_SIZE(true, AUDIO_SAMPLE_RATE, AUDIO_BYTES_PER_SMP, AUDIO_CHANNELS), 4),
};

static const char *s_string_desc[] = {
    (const char[]){0x09, 0x04}, /* English */
    "lunaticfringe",
    "P4 USB Display",
    "000001",
    "Display",
    "Touch",
    "P4 USB Display Speaker",
};

/* ---- Frame slots ---- */

#define NUM_SLOTS   3
#define CHUNK_BYTES 32768 /* multiple of 512; usbd_edpt_xfer takes uint16_t */
#define HDR_BUF     512

typedef enum { SLOT_FREE, SLOT_FILLING, SLOT_READY, SLOT_TAKEN } slot_state_t;

typedef struct {
    uint8_t     *data;
    uint32_t     len;
    uint32_t     seq;
    uint8_t      type;
    slot_state_t state;
} slot_t;

static slot_t s_slots[NUM_SLOTS];
static int s_ready = -1; /* newest complete frame, or -1 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_consumer;

/* ---- Receive state ---- */

typedef enum { RX_HEADER, RX_PAYLOAD, RX_DISCARD } rx_state_t;

static rx_state_t s_rx;
static uint8_t   *s_hdr_buf;
static uint8_t   *s_bounce[2];
static int        s_bidx;
static int        s_fill = -1;
static uint32_t   s_off, s_len, s_req;
static uint8_t    s_ep_out, s_ep_in;

static usb_link_stats_t s_stats;
static usb_link_stats_ext_cb_t s_stats_ext;
static usb_link_backlight_cb_t s_backlight_cb;
static uint8_t s_ctrl_buf[128]; /* must outlive the control transfer */

static void release_filling(void)
{
    if (s_fill >= 0) {
        portENTER_CRITICAL(&s_lock);
        s_slots[s_fill].state = SLOT_FREE;
        portEXIT_CRITICAL(&s_lock);
        s_fill = -1;
    }
}

static void arm(uint8_t rhport)
{
    uint8_t *dst;
    uint32_t n;

    switch (s_rx) {
    case RX_PAYLOAD:
    case RX_DISCARD:
        dst = s_bounce[s_bidx];
        n = s_len - s_off;
        break;
    case RX_HEADER:
    default:
        dst = s_hdr_buf;
        n = HDR_BUF;
        break;
    }
    if (n > CHUNK_BYTES) {
        n = CHUNK_BYTES;
    }
    s_req = n;
    usbd_edpt_xfer(rhport, s_ep_out, dst, (uint16_t)n, false);
}

static void on_header(uint32_t got)
{
    p4d_hdr_t h;
    if (got != sizeof(h)) {
        s_stats.bad_header++;
        return;
    }
    memcpy(&h, s_hdr_buf, sizeof(h));
    if (h.magic != P4D_MAGIC || h.len == 0) {
        s_stats.bad_header++;
        return;
    }

    s_len = h.len;
    s_off = 0;
    if (h.len > USB_LINK_SLOT_BYTES || h.type != P4D_TYPE_JPEG_FULL) {
        s_stats.oversize++;
        s_rx = RX_DISCARD;
        return;
    }

    int slot = -1;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < NUM_SLOTS; i++) {
        if (s_slots[i].state == SLOT_FREE) {
            slot = i;
            s_slots[i].state = SLOT_FILLING;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (slot < 0) {
        /* Cannot happen with three slots and one consumer, but never wedge. */
        s_rx = RX_DISCARD;
        return;
    }

    s_fill = slot;
    s_slots[slot].len = h.len;
    s_slots[slot].seq = h.seq;
    s_slots[slot].type = h.type;
    s_rx = RX_PAYLOAD;
}

static void on_payload_done(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_ready >= 0) {
        s_slots[s_ready].state = SLOT_FREE;
        s_stats.frames_stale++;
    }
    s_slots[s_fill].state = SLOT_READY;
    s_ready = s_fill;
    portEXIT_CRITICAL(&s_lock);

    s_fill = -1;
    s_stats.frames_rx++;
    if (s_consumer) {
        xTaskNotifyGive(s_consumer);
    }
}

/* ---- Class driver ---- */

static void drv_init(void)
{
}

static bool drv_deinit(void)
{
    return true;
}

static void drv_reset(uint8_t rhport)
{
    (void)rhport;
    release_filling();
    s_rx = RX_HEADER;
    s_ep_out = s_ep_in = 0;
}

static uint16_t drv_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len)
{
    if (itf->bInterfaceClass != TUSB_CLASS_VENDOR_SPECIFIC) {
        return 0;
    }
    uint16_t const len = sizeof(tusb_desc_interface_t) + itf->bNumEndpoints * sizeof(tusb_desc_endpoint_t);
    if (max_len < len ||
        !usbd_open_edpt_pair(rhport, tu_desc_next(itf), 2, TUSB_XFER_BULK, &s_ep_out, &s_ep_in)) {
        return 0;
    }
    release_filling();
    s_rx = RX_HEADER;
    arm(rhport);
    return len;
}

static bool drv_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t got)
{
    if (ep_addr != s_ep_out) {
        return true;
    }
    if (result != XFER_RESULT_SUCCESS) {
        release_filling();
        s_rx = RX_HEADER;
        arm(rhport);
        return true;
    }

    s_stats.rx_bytes += got;

    switch (s_rx) {
    case RX_HEADER:
        on_header(got);
        break;

    case RX_PAYLOAD:
    case RX_DISCARD: {
        const bool keep = s_rx == RX_PAYLOAD;
        const uint8_t *src = s_bounce[s_bidx];
        const uint32_t at = s_off;
        bool done = false, truncated = false;

        s_bidx ^= 1;
        s_off += got;
        if (got < s_req && s_off < s_len) {
            /* Short packet before len: the host sent less than it promised. */
            truncated = true;
            s_rx = RX_HEADER;
        } else if (s_off >= s_len) {
            done = true;
            s_rx = RX_HEADER;
        }

        /* Re-arm first so the next chunk lands in the other bounce buffer while
         * this one is copied out. */
        arm(rhport);

        if (truncated) {
            s_stats.truncated++;
            release_filling();
        } else if (keep) {
            uint8_t *slot = s_slots[s_fill].data;
            memcpy(slot + at, src, got);
            if (done) {
                /* No cache writeback here: jpeg_decoder_process() writes the
                 * input range back itself before its DMA starts (and the slots
                 * are not cache-aligned, so an explicit msync would be refused). */
                on_payload_done();
            }
        }
        return true;
    }
    }

    arm(rhport);
    return true;
}

static const usbd_class_driver_t s_driver = {
    .name = "p4disp",
    .init = drv_init,
    .deinit = drv_deinit,
    .reset = drv_reset,
    .open = drv_open,
    .control_xfer_cb = NULL, /* vendor requests arrive via tud_vendor_control_xfer_cb */
    .xfer_cb = drv_xfer_cb,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return &s_driver;
}

/* TinyUSB routes all vendor-type control requests here, never to a class driver. */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req)
{
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    switch (req->bRequest) {
    case P4D_REQ_GET_STATS: {
        uint16_t n = sizeof(s_stats);
        memcpy(s_ctrl_buf, &s_stats, n);
        if (s_stats_ext) {
            uint16_t ext = 0;
            s_stats_ext(s_ctrl_buf + n, &ext, sizeof(s_ctrl_buf) - n);
            n += ext;
        }
        return tud_control_xfer(rhport, req, s_ctrl_buf, n < req->wLength ? n : req->wLength);
    }
    case P4D_REQ_BACKLIGHT:
        if (s_backlight_cb) {
            s_backlight_cb(req->wValue);
        }
        return tud_control_status(rhport, req);
    default:
        return false; /* stall */
    }
}

/* ---- HID touchscreen (report layout lives in touch.c) ---- */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return touch_hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                               uint8_t *buf, uint16_t len)
{
    (void)instance;
    return type == HID_REPORT_TYPE_FEATURE ? touch_hid_get_feature(report_id, buf, len) : 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                           uint8_t const *buf, uint16_t len)
{
    /* Nothing settable: Windows' input-mode feature is not declared. */
}

void tud_mount_cb(void)
{
    ESP_LOGI(TAG, "host connected, %s speed",
             tud_speed_get() == TUSB_SPEED_HIGH ? "HIGH (480 Mbit/s)" : "FULL (12 Mbit/s)");
}

void tud_umount_cb(void)
{
    ESP_LOGI(TAG, "host disconnected");
}

/* ---- Public API ---- */

esp_err_t usb_link_init(void)
{
    s_consumer = xTaskGetCurrentTaskHandle();

    s_hdr_buf = heap_caps_aligned_alloc(CONFIG_CACHE_L1_CACHE_LINE_SIZE, HDR_BUF,
                                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    for (int i = 0; i < 2; i++) {
        s_bounce[i] = heap_caps_aligned_alloc(CONFIG_CACHE_L1_CACHE_LINE_SIZE, CHUNK_BYTES,
                                              MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    ESP_RETURN_ON_FALSE(s_hdr_buf && s_bounce[0] && s_bounce[1], ESP_ERR_NO_MEM, TAG, "rx buffers");

    /* The slots are the JPEG decoder's input, so allocate them the way it
     * wants: DMA-capable and cache-aligned. */
    jpeg_decode_memory_alloc_cfg_t mem = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    for (int i = 0; i < NUM_SLOTS; i++) {
        size_t cap = 0;
        s_slots[i].data = jpeg_alloc_decoder_mem(USB_LINK_SLOT_BYTES, &mem, &cap);
        ESP_RETURN_ON_FALSE(s_slots[i].data, ESP_ERR_NO_MEM, TAG, "slot %d", i);
        s_slots[i].state = SLOT_FREE;
    }

    const tinyusb_config_t cfg = {
        .device_descriptor = &s_device_desc,
        .string_descriptor = s_string_desc,
        .string_descriptor_count = sizeof(s_string_desc) / sizeof(s_string_desc[0]),
        .external_phy = false,
        .fs_configuration_descriptor = s_fs_config_desc,
        .hs_configuration_descriptor = s_hs_config_desc,
        .qualifier_descriptor = &s_qualifier_desc,
    };
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&cfg), TAG, "tinyusb");
    ESP_LOGI(TAG, "USB display link up (%d x %d KB slots)", NUM_SLOTS, USB_LINK_SLOT_BYTES / 1024);
    return ESP_OK;
}

bool usb_link_take(usb_link_frame_t *out, TickType_t wait)
{
    ulTaskNotifyTake(pdTRUE, wait);

    int slot;
    portENTER_CRITICAL(&s_lock);
    slot = s_ready;
    if (slot >= 0) {
        s_slots[slot].state = SLOT_TAKEN;
        s_ready = -1;
    }
    portEXIT_CRITICAL(&s_lock);
    if (slot < 0) {
        return false;
    }

    slot_t *s = &s_slots[slot];

    out->slot = slot;
    out->data = s->data;
    out->len = s->len;
    out->seq = s->seq;
    out->type = s->type;
    return true;
}

void usb_link_release(const usb_link_frame_t *f)
{
    portENTER_CRITICAL(&s_lock);
    s_slots[f->slot].state = SLOT_FREE;
    portEXIT_CRITICAL(&s_lock);
}

void usb_link_get_stats(usb_link_stats_t *out)
{
    *out = s_stats;
}

void usb_link_set_stats_ext(usb_link_stats_ext_cb_t cb)
{
    s_stats_ext = cb;
}

void usb_link_set_backlight_cb(usb_link_backlight_cb_t cb)
{
    s_backlight_cb = cb;
}
