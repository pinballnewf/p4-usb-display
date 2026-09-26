/*
 * USB High-Speed bring-up test for the Guition JC8012P4A1C.
 *
 * Enumerates the ESP32-P4's USB 2.0 HS OTG port (middle USB-C) as a vendor-class
 * device with one bulk OUT / bulk IN endpoint pair, then:
 *   - DMAs every byte the host writes to bulk OUT into ping-pong buffers, logs MB/s once a second
 *   - answers vendor control request 0x01 with an 8-byte little-endian total byte count
 *
 * Logs appear on the USB_UART / JTAG console, not on the HS port.
 */
#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "device/usbd_pvt.h"
#include "esp_heap_caps.h"

static const char *TAG = "usb_hs_test";

#define USB_VID 0x303A
#define USB_PID 0x4020 /* development PID, not allocated */

enum { ITF_VENDOR = 0, ITF_COUNT };
#define EP_OUT 0x01
#define EP_IN  0x81
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)

static const tusb_desc_device_t device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_VENDOR_SPECIFIC,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const tusb_desc_device_qualifier_t qualifier_desc = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_VENDOR_SPECIFIC,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 1,
    .bReserved = 0,
};

static const uint8_t fs_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_TOTAL_LEN, 0, 500),
    TUD_VENDOR_DESCRIPTOR(ITF_VENDOR, 4, EP_OUT, EP_IN, 64),
};

static const uint8_t hs_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_TOTAL_LEN, 0, 500),
    TUD_VENDOR_DESCRIPTOR(ITF_VENDOR, 4, EP_OUT, EP_IN, 512),
};

static const char *string_desc[] = {
    (const char[]){0x09, 0x04}, /* English */
    "lunaticfringe",
    "P4 USB Display (HS test)",
    "000001",
    "Display bulk",
};

static volatile uint64_t s_total_rx;

/*
 * App-level class driver: the DWC2 controller DMAs bulk OUT data straight into two
 * cache-aligned buffers (ping-pong), so nothing is copied through TinyUSB's FIFO.
 * Vendor control request REQ_GET_RX_TOTAL (IN, recipient=interface) returns the
 * 8-byte little-endian byte count.
 */
#define RX_BUF_SIZE      32768 /* multiple of 512; usbd_edpt_xfer takes uint16_t */
#define REQ_GET_RX_TOTAL 0x01

static uint8_t *s_rx_buf[2];
static uint8_t s_rx_idx;
static uint8_t s_ep_out, s_ep_in;
static uint64_t s_ctrl_total; /* must outlive the control transfer */

static void bulk_arm(uint8_t rhport)
{
    usbd_edpt_xfer(rhport, s_ep_out, s_rx_buf[s_rx_idx], RX_BUF_SIZE, false);
}

static void bulk_init(void)
{
    for (int i = 0; i < 2; i++) {
        s_rx_buf[i] = heap_caps_aligned_alloc(CONFIG_CACHE_L1_CACHE_LINE_SIZE, RX_BUF_SIZE,
                                              MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        assert(s_rx_buf[i]);
    }
}

static bool bulk_deinit(void)
{
    return true;
}

static void bulk_reset(uint8_t rhport)
{
    (void)rhport;
    s_ep_out = s_ep_in = 0;
}

static uint16_t bulk_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len)
{
    if (itf->bInterfaceClass != TUSB_CLASS_VENDOR_SPECIFIC) {
        return 0;
    }
    uint16_t const len = sizeof(tusb_desc_interface_t) + itf->bNumEndpoints * sizeof(tusb_desc_endpoint_t);
    if (max_len < len ||
        !usbd_open_edpt_pair(rhport, tu_desc_next(itf), 2, TUSB_XFER_BULK, &s_ep_out, &s_ep_in)) {
        return 0;
    }
    bulk_arm(rhport);
    return len;
}

static bool bulk_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req)
{
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    if (req->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR && req->bRequest == REQ_GET_RX_TOTAL) {
        s_ctrl_total = s_total_rx;
        return tud_control_xfer(rhport, req, &s_ctrl_total, sizeof(s_ctrl_total));
    }
    return false; /* stall anything else */
}

/* TinyUSB routes all vendor-type control requests here, never to a class driver. */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req)
{
    return bulk_control_xfer_cb(rhport, stage, req);
}

static bool bulk_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    if (ep_addr == s_ep_out && result == XFER_RESULT_SUCCESS) {
        s_total_rx += xferred_bytes;
        s_rx_idx ^= 1; /* hand the filled buffer to a consumer here, re-arm the other */
        bulk_arm(rhport);
    }
    return true;
}

static const usbd_class_driver_t s_bulk_driver = {
    .name = "p4disp",
    .init = bulk_init,
    .deinit = bulk_deinit,
    .reset = bulk_reset,
    .open = bulk_open,
    .control_xfer_cb = bulk_control_xfer_cb,
    .xfer_cb = bulk_xfer_cb,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return &s_bulk_driver;
}

static void stats_task(void *arg)
{
    uint64_t last_bytes = 0;
    int64_t last_us = esp_timer_get_time();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint64_t bytes = s_total_rx;
        int64_t now = esp_timer_get_time();
        if (bytes != last_bytes) {
            double mbps = (double)(bytes - last_bytes) / (double)(now - last_us); /* bytes/us == MB/s */
            ESP_LOGI(TAG, "rx %.2f MB/s (total %" PRIu64 " bytes)", mbps, bytes);
        }
        last_bytes = bytes;
        last_us = now;
    }
}

void tud_mount_cb(void)
{
    ESP_LOGI(TAG, "mounted, bus speed: %s",
             tud_speed_get() == TUSB_SPEED_HIGH ? "HIGH (480 Mbit/s)" : "FULL (12 Mbit/s)");
}

void tud_umount_cb(void)
{
    ESP_LOGI(TAG, "unmounted");
}

void app_main(void)
{
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = &device_desc,
        .string_descriptor = string_desc,
        .string_descriptor_count = sizeof(string_desc) / sizeof(string_desc[0]),
        .external_phy = false,
        .fs_configuration_descriptor = fs_config_desc,
        .hs_configuration_descriptor = hs_config_desc,
        .qualifier_descriptor = &qualifier_desc,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "TinyUSB started; plug the middle USB-C port into the host");
    xTaskCreate(stats_task, "stats", 4096, NULL, 5, NULL);
}
