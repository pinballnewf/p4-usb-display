/*
 * Display: internal LDO -> MIPI-DSI -> JD9365, no LVGL.
 *
 * Board bring-up is lifted from p4dash (bsp_display.c), which is proven on this
 * exact board. Read the traps section of p4dash/21.md before changing any of it;
 * the short version:
 *   - the panel is Guition's REVISED glass and needs the 204-command table in
 *     jd9365_guition_newpanel_init.h - every open-source JD9365 table leaves it
 *     near-black while it still answers DBI commands normally
 *   - stock 2-lane bus config and stock DPI timings, deliberately unmodified
 *   - mirror both axes (glass is mounted inverted relative to native scan)
 *
 * Frames are decoded straight into the single DPI frame buffer (p4tab's
 * approach): draw_bitmap on a DPI panel is asynchronous and its completion
 * handshake proved unreliable, whereas the panel rescans the buffer anyway.
 */

#include "bsp_display.h"

#include <string.h>

#include "driver/ledc.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_lcd_jd9365.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"

#include "bsp_pins.h"
#include "jd9365_guition_newpanel_init.h"

static const char *TAG = "bsp.disp";

#define BL_LEDC_TIMER    LEDC_TIMER_0
#define BL_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BL_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BL_LEDC_RES      LEDC_TIMER_10_BIT
/* 20 kHz keeps the backlight boost converter out of the audible band. */
#define BL_LEDC_FREQ_HZ  20000

static esp_ldo_channel_handle_t  s_phy_ldo;
static esp_lcd_panel_handle_t    s_panel;
static esp_lcd_panel_io_handle_t s_panel_io;
static uint16_t                 *s_fb;
static int                       s_backlight_pct;

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = BL_LEDC_MODE,
        .timer_num       = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_RES,
        .freq_hz         = BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");

    ledc_channel_config_t chan = {
        .gpio_num   = BSP_LCD_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .timer_sel  = BL_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    return ledc_channel_config(&chan);
}

esp_err_t bsp_display_backlight(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;

    uint32_t max_duty = (1u << BL_LEDC_RES) - 1;
    uint32_t duty = (max_duty * (uint32_t)percent) / 100;

    ESP_RETURN_ON_ERROR(ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty), TAG, "set duty");
    ESP_RETURN_ON_ERROR(ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL), TAG, "update duty");
    s_backlight_pct = percent;
    return ESP_OK;
}

int bsp_display_backlight_get(void)
{
    return s_backlight_pct;
}

esp_err_t bsp_display_init(void)
{
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");

    /* The DSI PHY runs off one of the P4's internal LDOs. Without this the DSI
     * bus init fails with an unhelpful timeout. */
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = BSP_LCD_LDO_PHY_CHAN,
        .voltage_mv = BSP_LCD_LDO_PHY_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &s_phy_ldo),
                        TAG, "DSI PHY LDO ch%d", BSP_LCD_LDO_PHY_CHAN);

    esp_lcd_dsi_bus_config_t bus_cfg = JD9365_PANEL_BUS_DSI_2CH_CONFIG();
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus), TAG, "dsi bus");

    esp_lcd_dbi_io_config_t dbi_cfg = JD9365_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &s_panel_io), TAG, "dbi io");

    esp_lcd_dpi_panel_config_t dpi_cfg =
        JD9365_800_1280_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);

    jd9365_vendor_config_t vendor_cfg = {
        .init_cmds      = jd9365_guition_newpanel_init_cmds,
        .init_cmds_size = JD9365_GUITION_NEWPANEL_INIT_CMDS_LEN,
        .mipi_config = {
            .dsi_bus    = dsi_bus,
            .dpi_config = &dpi_cfg,
            .lane_num   = BSP_LCD_DSI_LANES,
        },
    };

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config  = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9365(s_panel_io, &panel_cfg, &s_panel), TAG, "jd9365");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    /* Both axes mirrored: the glass is mounted inverted relative to the
     * driver's native scan direction. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, true), TAG, "mirror");
    /* Needed even though the init table ends with DISPON. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display on");

    void *fb = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb), TAG, "get frame buffer");
    s_fb = fb;
    memset(s_fb, 0, bsp_display_fb_bytes());
    esp_cache_msync(s_fb, bsp_display_fb_bytes(), ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    ESP_LOGI(TAG, "JD9365 up at %dx%d (revised-panel init, %u cmds), fb %p",
             BSP_LCD_H_RES, BSP_LCD_V_RES, (unsigned)JD9365_GUITION_NEWPANEL_INIT_CMDS_LEN, fb);
    return ESP_OK;
}

uint16_t *bsp_display_fb(void)              { return s_fb; }
size_t bsp_display_fb_bytes(void)           { return (size_t)BSP_LCD_H_RES * BSP_LCD_V_RES * 2; }
esp_lcd_panel_handle_t bsp_display_panel(void) { return s_panel; }
