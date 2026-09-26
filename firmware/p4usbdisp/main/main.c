/*
 * p4usbdisp - Guition JC8012P4A1C as a USB display.
 *
 * Host sends full-frame 4:2:0 JPEGs over the HS USB port (see usb_link.h);
 * the board decodes each into the panel's frame buffer with the hardware JPEG
 * engine. Full frames rather than tiles: p4tab measured the decoder at ~2.9 ms
 * fixed cost per call, so one 800x1280 decode (~10.5 ms) beats tiling for any
 * update touching more than ~12% of the screen.
 */

#include <inttypes.h>
#include <string.h>

#include "driver/jpeg_decode.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"

#include "bsp_display.h"
#include "bsp_pins.h"
#include "audio.h"
#include "touch.h"
#include "usb_link.h"

static const char *TAG = "p4usbdisp";

typedef struct __attribute__((packed)) {
    uint32_t frames_shown;
    uint32_t decode_fail;
    uint32_t decode_us_last;
    uint32_t decode_us_avg; /* over the last stats window */
} decode_stats_t;

static decode_stats_t s_dec;

static void stats_ext(uint8_t *buf, uint16_t *len, uint16_t max)
{
    if (max >= sizeof(s_dec)) {
        memcpy(buf, &s_dec, sizeof(s_dec));
        *len = sizeof(s_dec);
    }
}

static void backlight_cb(int percent)
{
    bsp_display_backlight(percent);
}

/* Portrait-raster test card so the panel can be checked before any USB
 * traffic: eight colour bars, a white border, and a white block in the
 * raster's top-left corner to show which way round the glass is mounted. */
static void draw_test_card(void)
{
    static const uint16_t bars[8] = {
        0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000,
    };
    uint16_t *fb = bsp_display_fb();
    const int w = BSP_LCD_H_RES, h = BSP_LCD_V_RES;

    for (int y = 0; y < h; y++) {
        uint16_t c = bars[(y * 8) / h];
        uint16_t *row = fb + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            bool border = x < 4 || x >= w - 4 || y < 4 || y >= h - 4;
            bool corner = x < 120 && y < 120;
            row[x] = (border || corner) ? 0xFFFF : c;
        }
    }
    esp_cache_msync(fb, bsp_display_fb_bytes(), ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

void app_main(void)
{
    ESP_ERROR_CHECK(bsp_display_init());
    draw_test_card();
    bsp_display_backlight(80);

    usb_link_set_stats_ext(stats_ext);
    usb_link_set_backlight_cb(backlight_cb);
    ESP_ERROR_CHECK(usb_link_init());

    /* Shared with the ES8311 codec. No external pull-ups on this board. */
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = BSP_I2C_PORT,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = BSP_I2C_INTERNAL_PULLUP,
    };
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));
    /* A missing or failed touch controller must not take the display down. */
    if (touch_init(i2c_bus) != ESP_OK) {
        ESP_LOGE(TAG, "touch unavailable - display only");
    }
    if (audio_init(i2c_bus) != ESP_OK) {
        ESP_LOGE(TAG, "audio unavailable");
    }

    jpeg_decoder_handle_t dec = NULL;
    /* Timeout well above the ~10.5 ms a frame takes, low enough that a
     * malformed frame does not stall the stream for long. */
    jpeg_decode_engine_cfg_t eng_cfg = { .intr_priority = 0, .timeout_ms = 200 };
    ESP_ERROR_CHECK(jpeg_new_decoder_engine(&eng_cfg, &dec));

    jpeg_decode_cfg_t dec_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        /* BGR, not RGB: with RGB red and blue come out swapped on this panel
         * (measured in p4tab). */
        .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std      = JPEG_YUV_RGB_CONV_STD_BT601,
    };

    uint8_t *fb = (uint8_t *)bsp_display_fb();
    const size_t fb_bytes = bsp_display_fb_bytes();

    int64_t win_start = esp_timer_get_time();
    uint32_t win_frames = 0;
    uint64_t win_dec_us = 0, win_bytes = 0;

    ESP_LOGI(TAG, "ready - waiting for frames on the middle USB-C port");

    for (;;) {
        usb_link_frame_t f;
        if (usb_link_take(&f, pdMS_TO_TICKS(1000))) {
            jpeg_decode_picture_info_t info;
            esp_err_t err = jpeg_decoder_get_info(f.data, f.len, &info);
            if (err == ESP_OK && (info.width != BSP_LCD_H_RES || info.height != BSP_LCD_V_RES)) {
                /* Decoding a different size straight into the frame buffer
                 * would scramble it; reject rather than draw garbage. */
                err = ESP_ERR_INVALID_SIZE;
            }

            int64_t t0 = esp_timer_get_time();
            uint32_t produced = 0;
            if (err == ESP_OK) {
                err = jpeg_decoder_process(dec, &dec_cfg, f.data, f.len, fb, fb_bytes, &produced);
            }
            int64_t t1 = esp_timer_get_time();
            usb_link_release(&f);

            if (err == ESP_OK) {
                /* The decoder wrote via DMA; flush so the panel's scan sees it. */
                esp_cache_msync(fb, fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                s_dec.frames_shown++;
                s_dec.decode_us_last = (uint32_t)(t1 - t0);
                win_frames++;
                win_dec_us += (uint64_t)(t1 - t0);
                win_bytes += f.len;
            } else {
                s_dec.decode_fail++;
                if (s_dec.decode_fail <= 5) {
                    ESP_LOGW(TAG, "frame %" PRIu32 " (%" PRIu32 " bytes) rejected: %s",
                             f.seq, f.len, esp_err_to_name(err));
                }
            }
        }

        int64_t now = esp_timer_get_time();
        if (now - win_start >= 2000000) {
            if (win_frames) {
                usb_link_stats_t ls;
                usb_link_get_stats(&ls);
                s_dec.decode_us_avg = (uint32_t)(win_dec_us / win_frames);
                double secs = (now - win_start) / 1e6;
                ESP_LOGI(TAG, "%.1f fps, decode %.1f ms, %.0f KB/frame | rx %" PRIu32
                         " stale %" PRIu32 " bad %" PRIu32 " trunc %" PRIu32 " fail %" PRIu32,
                         win_frames / secs, s_dec.decode_us_avg / 1000.0,
                         win_bytes / 1024.0 / win_frames, ls.frames_rx, ls.frames_stale,
                         ls.bad_header, ls.truncated, s_dec.decode_fail);
            }
            win_start = now;
            win_frames = 0;
            win_dec_us = win_bytes = 0;
        }
    }
}
