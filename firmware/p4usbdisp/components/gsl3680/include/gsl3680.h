/*
 * Silead GSL3680 capacitive touch controller.
 *
 * Deliberately not built on esp_lcd_touch: that abstraction assumes a panel-IO
 * transport with simple register reads, whereas the GSL3680 needs paged 4-byte
 * register writes and a 17 KB firmware upload. Talking to i2c_master directly
 * is less code than bending esp_lcd_touch around it.
 */

#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GSL3680_MAX_POINTS 5

typedef struct gsl3680_t *gsl3680_handle_t;

typedef struct {
    i2c_master_bus_handle_t bus;
    uint8_t     dev_addr;      /*!< 0x40 on this board */
    gpio_num_t  rst_gpio;
    gpio_num_t  int_gpio;      /*!< -1 to poll instead of using the INT line */
    uint16_t    x_max;         /*!< panel width in pixels  */
    uint16_t    y_max;         /*!< panel height in pixels */

    /* The controller does NOT report in screen pixels - its firmware has its
     * own coordinate space (roughly 0..1500 by 0..900 on this panel), so the
     * raw values need scaling as well as reorienting.
     *
     * Calibration is expressed as the raw reading observed at each end of a
     * screen axis, applied AFTER any swap. Putting the larger raw value in
     * *_at_min encodes an inverted axis, so no separate mirror flag is needed.
     *
     *   screen = (raw - raw_at_min) * (max - 1) / (raw_at_max - raw_at_min)
     */
    struct {
        int16_t x_raw_at_min;  /*!< raw value when the finger is at screen x = 0 */
        int16_t x_raw_at_max;  /*!< raw value when the finger is at x = x_max-1 */
        int16_t y_raw_at_min;
        int16_t y_raw_at_max;
    } cal;

    struct {
        unsigned int swap_xy: 1;   /*!< controller axes transposed vs the panel */
    } flags;
} gsl3680_config_t;

/* Resets the controller, uploads the firmware blob and verifies it took.
 * Takes roughly a second: 4356 I2C transactions at 400 kHz. */
esp_err_t gsl3680_new(const gsl3680_config_t *config, gsl3680_handle_t *ret_handle);

/* Reads the current touch point. *count is 0 when nothing is down.
 * Only the first finger is reported - see the note in gsl3680.c about the
 * vendor tracking algorithm. */
esp_err_t gsl3680_read(gsl3680_handle_t handle, uint16_t *x, uint16_t *y, uint8_t *count);

typedef struct {
    uint16_t x, y;   /*!< calibrated screen pixels (0..x_max-1, 0..y_max-1) */
    uint8_t  hw_id;  /*!< the controller's 4-bit finger tag; not a stable tracking ID */
} gsl3680_point_t;

/* Reads every finger the controller reports, calibrated like gsl3680_read().
 * *count is 0 when nothing is down. Points are in controller order, which is
 * not stable across frames - match them to fingers yourself. */
esp_err_t gsl3680_read_points(gsl3680_handle_t handle, gsl3680_point_t pts[GSL3680_MAX_POINTS],
                              uint8_t *count);

/* Raw, untransformed coordinates straight off the controller. For the touch
 * test screen, where seeing the pre-transform values is the whole point. */
esp_err_t gsl3680_read_raw(gsl3680_handle_t handle, uint16_t *x, uint16_t *y, uint8_t *count);

#ifdef __cplusplus
}
#endif
