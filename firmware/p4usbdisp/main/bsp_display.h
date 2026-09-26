#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

/* Brings up the DSI PHY rail and the JD9365 panel, and clears the frame buffer
 * to black. Backlight is left off; call bsp_display_backlight() once there is
 * something worth showing. */
esp_err_t bsp_display_init(void);

/* 0..100. Non-linear perception is not corrected for here. */
esp_err_t bsp_display_backlight(int percent);
int bsp_display_backlight_get(void);

/* The DPI frame buffer the panel scans continuously: BSP_LCD_H_RES x
 * BSP_LCD_V_RES RGB565, native portrait raster. Writers must
 * esp_cache_msync(..., C2M) afterwards or the panel scans stale data. */
uint16_t *bsp_display_fb(void);
size_t bsp_display_fb_bytes(void);

esp_lcd_panel_handle_t bsp_display_panel(void);
