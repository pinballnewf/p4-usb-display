#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

/*
 * GSL3680 touch -> USB HID multi-touch touchscreen.
 *
 * Coordinates are reported in the panel's native portrait raster (800x1280)
 * with its physical size, so the host maps them onto the monitor it belongs to
 * and applies that monitor's rotation itself - nothing here depends on how the
 * desktop is oriented.
 */

#define TOUCH_MAX_CONTACTS   5
#define TOUCH_REPORT_ID      1 /* input: contacts */
#define TOUCH_FEATURE_MAX_ID 2 /* feature: contact count maximum */

/* Must be a constant for the configuration descriptor; touch.c asserts it. */
#define TOUCH_HID_REPORT_DESC_LEN 364

extern const uint8_t touch_hid_report_desc[];
extern const size_t  touch_hid_report_desc_len;

/* Uploads the controller firmware (~1 s) and starts the polling task. */
esp_err_t touch_init(i2c_master_bus_handle_t bus);

/* Answers HID GET_REPORT for the feature report. Returns bytes written. */
uint16_t touch_hid_get_feature(uint8_t report_id, uint8_t *buf, uint16_t len);
