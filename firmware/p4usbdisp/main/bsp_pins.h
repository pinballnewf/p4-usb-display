/*
 * Board support pin map: Guition JC8012P4A1C_I_W_Y
 * 10.1" 800x1280 IPS MIPI-DSI panel, ESP32-P4 + ESP32-C6.
 *
 * Every assignment below was read off the official schematics in
 * github.com/p1ngb4ck/unofficial_guition_esp32p4_repo/JC8012P4A1C_I_W_Y.
 * Sheet names are cited per block so they can be re-checked.
 *
 * Board facts (specification/JC8012P4A1C_I_W Specifications-EN-v1.1):
 *   ESP32-P4 @ 400 MHz, 768 KB L2MEM, 32 MB PSRAM (hex, 200 MHz), 16 MB flash
 *   Panel driver IC JD9365, 800x1280, IPS, 216.58 x 135.36 mm active area
 *   SKU _Y has no battery cell fitted; _Y1 ships with one. Charger is populated
 *   either way, so the sense divider reads a floating node on a _Y board.
 */

#pragma once

#include "driver/gpio.h"

/* ---- Display: JD9365 over 2-lane MIPI-DSI (sheet 2_LCD, 3_ESP32-P4) ---- */
/* Physical DPI timing / frame buffer - the panel's native raster. Never
 * changes with UI orientation; see BSP_UI_W/H for that. */
#define BSP_LCD_H_RES              800
#define BSP_LCD_V_RES              1280

/* Logical UI resolution after the landscape rotation applied in
 * bsp_display_init() (LV_DISPLAY_ROTATION_90). UI code should lay out
 * against these, not BSP_LCD_H_RES/V_RES. */
#define BSP_UI_W                   1280
#define BSP_UI_H                   800
#define BSP_LCD_DSI_LANES          2
/* Informational only - we use the component's stock 2CH bus config rather than
 * overriding this. Kept as a tuning knob if the link ever needs slowing. */
#define BSP_LCD_DSI_LANE_MBPS      1500
/* DSI PHY rail comes off the P4's internal LDO, not a discrete regulator.
 * ESPHome's working config uses channel 3 at 2.5 V. */
#define BSP_LCD_LDO_PHY_CHAN       3
#define BSP_LCD_LDO_PHY_MV         2500
#define BSP_LCD_RST                GPIO_NUM_27   /* pin 57, net GPIO27      */
#define BSP_LCD_BL                 GPIO_NUM_23   /* pin 25, net LCD_PWM     */

/* ---- Touch: GSL3680 capacitive controller (sheet 4_CONN) ----
 * Shares the I2C bus with the ES8311 codec. Needs a ~30 KB firmware blob
 * pushed over I2C on every cold boot before it reports coordinates. */
#define BSP_TOUCH_INT              GPIO_NUM_21   /* pin 23, net TOUCH_INT   */
#define BSP_TOUCH_RST              GPIO_NUM_22   /* pin 24, net TOUCH_RST   */
#define BSP_TOUCH_I2C_ADDR         0x40

/* ---- Shared I2C bus (sheet 7_CODEC) ---- */
#define BSP_I2C_PORT               I2C_NUM_0
#define BSP_I2C_SDA                GPIO_NUM_7    /* pin 7, net ES_I2C_SDA   */
#define BSP_I2C_SCL                GPIO_NUM_8    /* pin 8, net ES_I2C_SCL   */
#define BSP_I2C_FREQ_HZ            400000
/* Board has no external pull-ups on this bus - the ESPHome config explicitly
 * disables them and relies on the internal ones. Match that. */
#define BSP_I2C_INTERNAL_PULLUP    1

/* ---- Audio: ES8311 codec + NS4150 class-D amp (sheet 7_CODEC) ----
 * Schematic reuses ES7210_* net names for the ES8311's I2S pins; there is no
 * ES7210 populated on this board. Mic is a single analog MSM381A3729H9CP
 * into the ES8311's MIC1 input, so capture is mono. */
#define BSP_I2S_PORT               I2S_NUM_0
#define BSP_I2S_MCLK               GPIO_NUM_13   /* net CODEC_I2S0_MCLK     */
#define BSP_I2S_BCLK               GPIO_NUM_12   /* net ES7210_SCLK         */
#define BSP_I2S_WS                 GPIO_NUM_10   /* net ES7210_LRCK         */
#define BSP_I2S_DOUT               GPIO_NUM_9    /* net CODEC_I2S0_DSDIN    */
#define BSP_I2S_DIN                GPIO_NUM_11   /* net ES7210_SDOUT        */
#define BSP_PA_CTRL                GPIO_NUM_20   /* pin 22, net PA_CTRL     */
#define BSP_ES8311_I2C_ADDR        0x18          /* verify via bus scan     */

/* ---- WiFi: ESP32-C6 slave over SDIO, esp_hosted (sheet 5_ESP32-C6) ----
 * These are the P4's SD2 peripheral pins wired to the C6, not to the TF slot;
 * the microSD card sits on the dedicated SDMMC slot-0 pins instead. */
#define BSP_C6_SDIO_CLK            GPIO_NUM_18   /* net SD2_CLK             */
#define BSP_C6_SDIO_CMD            GPIO_NUM_19   /* net SD2_CMD             */
#define BSP_C6_SDIO_D0             GPIO_NUM_14   /* net SD2_D0              */
#define BSP_C6_SDIO_D1             GPIO_NUM_15   /* net SD2_D1              */
#define BSP_C6_SDIO_D2             GPIO_NUM_16   /* net SD2_D2              */
#define BSP_C6_SDIO_D3             GPIO_NUM_17   /* net SD2_D3              */
#define BSP_C6_RST                 GPIO_NUM_54   /* active high per ESPHome */

/* ---- Battery sense (sheet 1_PWR) ----
 * IP5306 charger/boost. BAT+ divides through R2 68K / R6 100K into GPIO52
 * via a 0R link. Vadc = Vbat * 100/168, so Vbat = Vadc * 1.68.
 * A full 4.2 V cell lands at 2.50 V on the pin, inside 12 dB attenuation range.
 * The IP5306 here is the non-I2C variant, so there is no fuel gauge to read -
 * voltage is all we get. Resolve the ADC unit/channel at runtime with
 * adc_oneshot_io_to_channel() rather than hardcoding a guess. */
#define BSP_BAT_ADC_GPIO           GPIO_NUM_52
#define BSP_BAT_DIV_NUM            168           /* R2 + R6 */
#define BSP_BAT_DIV_DEN            100           /* R6      */

/* ---- Misc (sheet 3_ESP32-P4) ---- */
#define BSP_WS2812                 GPIO_NUM_26   /* pin 55, net WS2812_DAT  */
#define BSP_UART0_TX               GPIO_NUM_37
#define BSP_UART0_RX               GPIO_NUM_38
