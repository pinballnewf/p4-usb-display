#include "gsl3680.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gsl3680_fw.h"

static const char *TAG = "gsl3680";

/* Register map, such as it is. The GSL parts are barely documented; these come
 * from the vendor Linux driver and the ESPHome port that works on this board. */
#define REG_TOUCH_DATA   0x80
#define REG_STATUS       0xb0   /* reads 0x5a5a5a5a once firmware is running */
#define REG_RESET_A      0xbc
#define REG_CLOCK        0xe0
#define REG_POWER        0xe4
#define REG_PAGE         0xf0

struct gsl3680_t {
    i2c_master_dev_handle_t dev;
    gpio_num_t rst_gpio;
    gpio_num_t int_gpio;
    uint16_t   x_max;
    uint16_t   y_max;
    struct {
        int16_t x_raw_at_min, x_raw_at_max;
        int16_t y_raw_at_min, y_raw_at_max;
    } cal;
    struct {
        unsigned int swap_xy: 1;
    } flags;
};

static esp_err_t reg_write(gsl3680_handle_t h, uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[5];
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(h->dev, buf, len + 1, 100);
}

static esp_err_t reg_write_u8(gsl3680_handle_t h, uint8_t reg, uint8_t val)
{
    return reg_write(h, reg, &val, 1);
}

static esp_err_t reg_read(gsl3680_handle_t h, uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(h->dev, &reg, 1, data, len, 100);
}

/* Probe: write a known pattern to the page register and read it back. If the
 * controller is absent or held in reset this is where we find out, rather than
 * a thousand writes later. */
static esp_err_t check_alive(gsl3680_handle_t h)
{
    uint8_t probe[4] = {0x12, 0x34, 0x56, 0x00};
    uint8_t buf[4] = {0};

    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(reg_read(h, REG_PAGE, buf, 4), TAG, "read page");
    ESP_LOGD(TAG, "page before: %02x %02x %02x %02x", buf[0], buf[1], buf[2], buf[3]);

    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(reg_write(h, REG_PAGE, probe, 4), TAG, "write page");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(reg_read(h, REG_PAGE, buf, 4), TAG, "read page back");
    ESP_LOGD(TAG, "page after:  %02x %02x %02x %02x", buf[0], buf[1], buf[2], buf[3]);

    ESP_RETURN_ON_FALSE(buf[0] == probe[0], ESP_ERR_NOT_FOUND, TAG,
                        "controller not responding (page read back 0x%02x, want 0x12)",
                        buf[0]);
    return ESP_OK;
}

static esp_err_t clear_registers(gsl3680_handle_t h)
{
    static const uint8_t regs[4] = {REG_CLOCK, 0x88, REG_POWER, REG_CLOCK};
    static const uint8_t vals[4] = {0x88, 0x01, 0x04, 0x00};

    for (int i = 0; i < 4; i++) {
        ESP_RETURN_ON_ERROR(reg_write_u8(h, regs[i], vals[i]), TAG, "clear %d", i);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

static esp_err_t chip_reset(gsl3680_handle_t h)
{
    gpio_set_level(h->rst_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(h->rst_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_RETURN_ON_ERROR(reg_write_u8(h, REG_CLOCK, 0x88), TAG, "clock");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(reg_write_u8(h, REG_POWER, 0x04), TAG, "power");
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t zero[4] = {0, 0, 0, 0};
    ESP_RETURN_ON_ERROR(reg_write(h, REG_RESET_A, zero, 4), TAG, "reset a");
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

static esp_err_t load_firmware(gsl3680_handle_t h)
{
    int64_t t0 = esp_timer_get_time();

    for (size_t i = 0; i < GSL3680_FW_LEN; i++) {
        uint8_t reg = GSL3680_FW[i].offset;
        uint32_t v  = GSL3680_FW[i].val;
        uint8_t payload[4] = {
            (uint8_t)(v & 0xff),
            (uint8_t)((v >> 8) & 0xff),
            (uint8_t)((v >> 16) & 0xff),
            (uint8_t)((v >> 24) & 0xff),
        };
        /* The page-select register is the one that takes a single byte. */
        size_t len = (reg == GSL3680_FW_PAGE_SELECT_REG) ? 1 : 4;
        esp_err_t err = reg_write(h, reg, payload, len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "firmware upload failed at entry %u/%u (reg 0x%02x): %s",
                     (unsigned)i, (unsigned)GSL3680_FW_LEN, reg, esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "firmware uploaded: %u entries in %lld ms",
             (unsigned)GSL3680_FW_LEN, (esp_timer_get_time() - t0) / 1000);
    return ESP_OK;
}

/* Once running, the status register reads 0x5a in all four bytes. Anything else
 * means the upload was corrupted or the blob does not match this silicon. */
static esp_err_t verify_running(gsl3680_handle_t h)
{
    uint8_t buf[4] = {0};
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_RETURN_ON_ERROR(reg_read(h, REG_STATUS, buf, 4), TAG, "status");

    for (int i = 0; i < 4; i++) {
        if (buf[i] != 0x5a) {
            ESP_LOGE(TAG, "status %02x %02x %02x %02x, expected 5a 5a 5a 5a",
                     buf[0], buf[1], buf[2], buf[3]);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    return ESP_OK;
}

esp_err_t gsl3680_new(const gsl3680_config_t *config, gsl3680_handle_t *ret_handle)
{
    ESP_RETURN_ON_FALSE(config && ret_handle, ESP_ERR_INVALID_ARG, TAG, "null arg");

    esp_err_t ret = ESP_OK;   /* ESP_GOTO_ON_ERROR writes through this name */

    gsl3680_handle_t h = calloc(1, sizeof(struct gsl3680_t));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "no mem");

    h->rst_gpio     = config->rst_gpio;
    h->int_gpio     = config->int_gpio;
    h->x_max        = config->x_max;
    h->y_max        = config->y_max;
    h->flags.swap_xy  = config->flags.swap_xy;
    /* Field by field: the two anonymous struct types are distinct to the
     * compiler even though the members match. */
    h->cal.x_raw_at_min = config->cal.x_raw_at_min;
    h->cal.x_raw_at_max = config->cal.x_raw_at_max;
    h->cal.y_raw_at_min = config->cal.y_raw_at_min;
    h->cal.y_raw_at_max = config->cal.y_raw_at_max;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = config->dev_addr,
        .scl_speed_hz    = 400000,
    };
    ret = i2c_master_bus_add_device(config->bus, &dev_cfg, &h->dev);
    if (ret != ESP_OK) {
        free(h);
        ESP_RETURN_ON_ERROR(ret, TAG, "add i2c device");
    }

    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << config->rst_gpio,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_GOTO_ON_ERROR(gpio_config(&rst_cfg), fail, TAG, "reset gpio");

    if (config->int_gpio >= 0) {
        gpio_config_t int_cfg = {
            .pin_bit_mask = 1ULL << config->int_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&int_cfg), fail, TAG, "int gpio");
    }

    /* Release reset before the first transaction - the controller holds the
     * bus otherwise, which is why a cold I2C scan finds nothing at 0x40. */
    gpio_set_level(config->rst_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(config->rst_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_GOTO_ON_ERROR(check_alive(h),     fail, TAG, "probe");
    ESP_GOTO_ON_ERROR(clear_registers(h), fail, TAG, "clear");
    ESP_GOTO_ON_ERROR(chip_reset(h),      fail, TAG, "reset");
    ESP_GOTO_ON_ERROR(load_firmware(h),   fail, TAG, "firmware");
    ESP_GOTO_ON_ERROR(reg_write_u8(h, REG_CLOCK, 0x00), fail, TAG, "start");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_GOTO_ON_ERROR(verify_running(h),  fail, TAG, "verify");

    ESP_LOGI(TAG, "ready (%ux%u)", h->x_max, h->y_max);
    *ret_handle = h;
    return ESP_OK;

fail:
    i2c_master_bus_rm_device(h->dev);
    free(h);
    return ESP_FAIL;
}

/*
 * The vendor ships an 80 KB "point id" tracking algorithm alongside this, which
 * smooths coordinates and assigns stable per-finger IDs. We read raw positions
 * instead: for a single finger driving an LVGL UI it is indistinguishable, and
 * it keeps a large lump of opaque vendor code out of the build. If per-finger
 * tracking is ever needed, gsl_point_id.cpp is in the same upstream component
 * as the firmware blob.
 *
 * `count` is the controller's real finger count, but only the FIRST point's
 * coordinates are decoded here. gsl3680_read_points() decodes all of them; the
 * caller does its own finger tracking (p4usbdisp: nearest-neighbour in touch.c).
 */
esp_err_t gsl3680_read_raw(gsl3680_handle_t handle, uint16_t *x, uint16_t *y, uint8_t *count)
{
    ESP_RETURN_ON_FALSE(handle && x && y && count, ESP_ERR_INVALID_ARG, TAG, "null arg");

    uint8_t data[24];
    ESP_RETURN_ON_ERROR(reg_read(handle, REG_TOUCH_DATA, data, sizeof(data)),
                        TAG, "read touch data");

    uint8_t fingers = data[0];
    if (fingers == 0 || fingers > GSL3680_MAX_POINTS) {
        *count = 0;
        return ESP_OK;
    }

    /* First point lives at bytes 4..7: y in 4/5, x in 6/7. The top nibble of
     * byte 7 carries the finger ID rather than coordinate data and has always
     * been masked here; byte 5's top nibble was not, and occasionally comes
     * back non-zero - a real y of 84 (0x0054) was observed reading as 16468
     * (0x4054) on the first sample after touch-down. Both axes top out around
     * 1500, well inside 12 bits, so masking can only strip garbage. */
    *y = (uint16_t)(((data[5] & 0x0f) << 8) | data[4]);
    *x = (uint16_t)(((data[7] & 0x0f) << 8) | data[6]);
    *count = fingers;
    return ESP_OK;
}

/* Map a raw reading onto 0..(size-1), clamping at both ends. at_min > at_max
 * simply inverts the axis, which is how a reversed sensor orientation is
 * expressed rather than with a separate mirror flag. */
static int32_t map_axis(int32_t raw, int32_t at_min, int32_t at_max, int32_t size)
{
    int32_t span = at_max - at_min;
    if (span == 0) {
        return 0;
    }
    int32_t v = ((raw - at_min) * (size - 1)) / span;
    if (v < 0)         v = 0;
    if (v > size - 1)  v = size - 1;
    return v;
}

esp_err_t gsl3680_read(gsl3680_handle_t handle, uint16_t *x, uint16_t *y, uint8_t *count)
{
    ESP_RETURN_ON_ERROR(gsl3680_read_raw(handle, x, y, count), TAG, "raw read");
    if (*count == 0) {
        return ESP_OK;
    }

    int32_t rx = *x, ry = *y;

    if (handle->flags.swap_xy) {
        int32_t t = rx; rx = ry; ry = t;
    }

    /* Linear map from the controller's own coordinate space onto screen
     * pixels. A calibration pair with at_min > at_max inverts that axis. */
    int32_t sx = map_axis(rx, handle->cal.x_raw_at_min, handle->cal.x_raw_at_max,
                          handle->x_max);
    int32_t sy = map_axis(ry, handle->cal.y_raw_at_min, handle->cal.y_raw_at_max,
                          handle->y_max);

    *x = (uint16_t)sx;
    *y = (uint16_t)sy;
    return ESP_OK;
}

esp_err_t gsl3680_read_points(gsl3680_handle_t handle, gsl3680_point_t pts[GSL3680_MAX_POINTS],
                              uint8_t *count)
{
    ESP_RETURN_ON_FALSE(handle && pts && count, ESP_ERR_INVALID_ARG, TAG, "null arg");

    /* 4-byte header (finger count in byte 0) then 4 bytes per point: y in 0/1,
     * x in 2/3, finger tag in the top nibble of byte 3. Top nibbles of both
     * high bytes masked for the same reason as in gsl3680_read_raw(). */
    uint8_t data[4 + 4 * GSL3680_MAX_POINTS];
    ESP_RETURN_ON_ERROR(reg_read(handle, REG_TOUCH_DATA, data, sizeof(data)),
                        TAG, "read touch data");

    uint8_t n = data[0];
    if (n > GSL3680_MAX_POINTS) {
        n = 0;
    }
    for (uint8_t i = 0; i < n; i++) {
        const uint8_t *p = &data[4 + 4 * i];
        int32_t rx = ((p[3] & 0x0f) << 8) | p[2];
        int32_t ry = ((p[1] & 0x0f) << 8) | p[0];
        if (handle->flags.swap_xy) {
            int32_t t = rx; rx = ry; ry = t;
        }
        pts[i].x = (uint16_t)map_axis(rx, handle->cal.x_raw_at_min, handle->cal.x_raw_at_max,
                                      handle->x_max);
        pts[i].y = (uint16_t)map_axis(ry, handle->cal.y_raw_at_min, handle->cal.y_raw_at_max,
                                      handle->y_max);
        pts[i].hw_id = p[3] >> 4;
    }
    *count = n;
    return ESP_OK;
}
