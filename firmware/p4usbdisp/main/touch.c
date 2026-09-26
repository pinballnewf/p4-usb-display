/*
 * Touch: GSL3680 -> finger tracking -> USB HID multi-touch touchscreen.
 *
 * The controller reports up to five points per poll, in no stable order, and
 * its 4-bit finger tag is not a reliable identity (the vendor's answer is an
 * 80 KB tracking library). HID multi-touch needs a stable contact ID per finger
 * for gestures to work, so a small nearest-neighbour tracker here assigns them:
 * each existing contact claims the closest new point within MAX_JUMP_PX,
 * leftovers become new contacts, and contacts with no point are lifted.
 *
 * Tracker state only advances when a report has actually been queued, so a
 * busy or absent host can never swallow a lift and leave a finger stuck down.
 */

#include "touch.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gsl3680.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"

#include "bsp_pins.h"

static const char *TAG = "touch";

#define POLL_MS     8   /* 125 Hz, as p4tab used */
#define MAX_JUMP_PX 250 /* a finger cannot move further than this in one poll */

/* Physical size in 0.1 mm (unit cm, exponent -2): 135.36 x 216.58 mm. */
#define PHYS_X_MAX 1354
#define PHYS_Y_MAX 2166

#define LE16(v) ((v) & 0xFF), (((v) >> 8) & 0xFF)

#define FINGER_COLLECTION                                                        \
    0x05, 0x0D,                   /*   Usage Page (Digitizer)              */   \
    0x09, 0x22,                   /*   Usage (Finger)                      */   \
    0xA1, 0x02,                   /*   Collection (Logical)                */   \
    0x09, 0x42,                   /*     Usage (Tip Switch)                */   \
    0x15, 0x00,                   /*     Logical Minimum (0)               */   \
    0x25, 0x01,                   /*     Logical Maximum (1)               */   \
    0x75, 0x01,                   /*     Report Size (1)                   */   \
    0x95, 0x01,                   /*     Report Count (1)                  */   \
    0x81, 0x02,                   /*     Input (Data,Var,Abs)              */   \
    0x75, 0x07,                   /*     Report Size (7)                   */   \
    0x81, 0x03,                   /*     Input (Const) - padding           */   \
    0x09, 0x51,                   /*     Usage (Contact Identifier)        */   \
    0x25, 0x7F,                   /*     Logical Maximum (127)             */   \
    0x75, 0x08,                   /*     Report Size (8)                   */   \
    0x81, 0x02,                   /*     Input (Data,Var,Abs)              */   \
    0x05, 0x01,                   /*     Usage Page (Generic Desktop)      */   \
    0x75, 0x10,                   /*     Report Size (16)                  */   \
    0x55, 0x0E,                   /*     Unit Exponent (-2)                */   \
    0x65, 0x11,                   /*     Unit (cm, SI linear)              */   \
    0x35, 0x00,                   /*     Physical Minimum (0)              */   \
    0x26, LE16(BSP_LCD_H_RES - 1),/*     Logical Maximum (799)             */   \
    0x46, LE16(PHYS_X_MAX),       /*     Physical Maximum                  */   \
    0x09, 0x30,                   /*     Usage (X)                         */   \
    0x81, 0x02,                   /*     Input (Data,Var,Abs)              */   \
    0x26, LE16(BSP_LCD_V_RES - 1),/*     Logical Maximum (1279)            */   \
    0x46, LE16(PHYS_Y_MAX),       /*     Physical Maximum                  */   \
    0x09, 0x31,                   /*     Usage (Y)                         */   \
    0x81, 0x02,                   /*     Input (Data,Var,Abs)              */   \
    0x65, 0x00,                   /*     Unit (none)                       */   \
    0x55, 0x00,                   /*     Unit Exponent (0)                 */   \
    0x45, 0x00,                   /*     Physical Maximum (0)              */   \
    0xC0                          /*   End Collection                      */

const uint8_t touch_hid_report_desc[] = {
    0x05, 0x0D,                   /* Usage Page (Digitizer)                */
    0x09, 0x04,                   /* Usage (Touch Screen)                  */
    0xA1, 0x01,                   /* Collection (Application)              */
    0x85, TOUCH_REPORT_ID,        /*   Report ID                           */
    FINGER_COLLECTION,
    FINGER_COLLECTION,
    FINGER_COLLECTION,
    FINGER_COLLECTION,
    FINGER_COLLECTION,
    0x05, 0x0D,                   /*   Usage Page (Digitizer)              */
    0x09, 0x54,                   /*   Usage (Contact Count)               */
    0x15, 0x00,                   /*   Logical Minimum (0)                 */
    0x25, TOUCH_MAX_CONTACTS,     /*   Logical Maximum                     */
    0x75, 0x08,                   /*   Report Size (8)                     */
    0x95, 0x01,                   /*   Report Count (1)                    */
    0x81, 0x02,                   /*   Input (Data,Var,Abs)                */
    0x85, TOUCH_FEATURE_MAX_ID,   /*   Report ID                           */
    0x09, 0x55,                   /*   Usage (Contact Count Maximum)       */
    0xB1, 0x02,                   /*   Feature (Data,Var,Abs)              */
    0xC0                          /* End Collection                        */
};
const size_t touch_hid_report_desc_len = sizeof(touch_hid_report_desc);
_Static_assert(sizeof(touch_hid_report_desc) == TOUCH_HID_REPORT_DESC_LEN,
               "update TOUCH_HID_REPORT_DESC_LEN in touch.h");

typedef struct __attribute__((packed)) {
    uint8_t  tip;  /* bit 0 */
    uint8_t  id;
    uint16_t x;
    uint16_t y;
} hid_contact_t;

typedef struct __attribute__((packed)) {
    hid_contact_t c[TOUCH_MAX_CONTACTS];
    uint8_t       count;
} hid_report_t;

typedef struct {
    bool     active;
    uint8_t  id;
    uint16_t x, y;
} contact_t;

static gsl3680_handle_t s_gsl;
static contact_t s_contacts[TOUCH_MAX_CONTACTS];
static uint8_t s_next_id;

uint16_t touch_hid_get_feature(uint8_t report_id, uint8_t *buf, uint16_t len)
{
    if (report_id == TOUCH_FEATURE_MAX_ID && len >= 1) {
        buf[0] = TOUCH_MAX_CONTACTS;
        return 1;
    }
    return 0;
}

static uint32_t dist2(uint16_t ax, uint16_t ay, uint16_t bx, uint16_t by)
{
    int32_t dx = (int32_t)ax - bx, dy = (int32_t)ay - by;
    return (uint32_t)(dx * dx + dy * dy);
}

/* Matches this poll's points to the tracked contacts, writing the next contact
 * state into `next` and the HID report for the transition into `rep`.
 * Returns false when there is nothing to report (idle, no fingers). */
static bool track(const gsl3680_point_t *pts, uint8_t n, contact_t *next, hid_report_t *rep)
{
    bool point_used[GSL3680_MAX_POINTS] = {0};
    bool contact_matched[TOUCH_MAX_CONTACTS] = {0};
    memcpy(next, s_contacts, sizeof(s_contacts));
    memset(rep, 0, sizeof(*rep));

    /* Greedy global nearest-neighbour: repeatedly take the closest remaining
     * (contact, point) pair. With at most 5x5 candidates this is trivial. */
    for (;;) {
        uint32_t best = (uint32_t)MAX_JUMP_PX * MAX_JUMP_PX + 1;
        int bc = -1, bp = -1;
        for (int c = 0; c < TOUCH_MAX_CONTACTS; c++) {
            if (!next[c].active || contact_matched[c]) {
                continue;
            }
            for (int p = 0; p < n; p++) {
                if (point_used[p]) {
                    continue;
                }
                uint32_t d = dist2(next[c].x, next[c].y, pts[p].x, pts[p].y);
                if (d < best) {
                    best = d;
                    bc = c;
                    bp = p;
                }
            }
        }
        if (bc < 0) {
            break;
        }
        contact_matched[bc] = true;
        point_used[bp] = true;
        next[bc].x = pts[bp].x;
        next[bc].y = pts[bp].y;
    }

    /* Unmatched points become new contacts. */
    for (int p = 0; p < n; p++) {
        if (point_used[p]) {
            continue;
        }
        for (int c = 0; c < TOUCH_MAX_CONTACTS; c++) {
            if (!next[c].active) {
                next[c] = (contact_t){ .active = true, .id = s_next_id, .x = pts[p].x, .y = pts[p].y };
                s_next_id = (s_next_id + 1) & 0x7F;
                contact_matched[c] = true;
                break;
            }
        }
    }

    /* Report every contact that is down, plus one final tip-up for each that
     * just lifted (those go inactive in `next`). */
    uint8_t k = 0;
    for (int c = 0; c < TOUCH_MAX_CONTACTS; c++) {
        if (!next[c].active) {
            continue;
        }
        bool down = contact_matched[c];
        rep->c[k++] = (hid_contact_t){ .tip = down, .id = next[c].id, .x = next[c].x, .y = next[c].y };
        if (!down) {
            next[c].active = false;
        }
    }
    rep->count = k;
    return k > 0;
}

static void touch_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(POLL_MS));

        gsl3680_point_t pts[GSL3680_MAX_POINTS];
        uint8_t n = 0;
        if (gsl3680_read_points(s_gsl, pts, &n) != ESP_OK) {
            continue;
        }

        contact_t next[TOUCH_MAX_CONTACTS];
        hid_report_t rep;
        if (!track(pts, n, next, &rep)) {
            continue;
        }
        /* Only commit the new state once the report is queued; otherwise try
         * the same transition again next poll. */
        if (tud_mounted() && tud_hid_ready() &&
            tud_hid_report(TOUCH_REPORT_ID, &rep, sizeof(rep))) {
            memcpy(s_contacts, next, sizeof(s_contacts));
        } else if (!tud_mounted()) {
            memset(s_contacts, 0, sizeof(s_contacts)); /* no host: nothing is "down" */
        }
    }
}

esp_err_t touch_init(i2c_master_bus_handle_t bus)
{
    /* Calibration from p4dash bsp_touch.c: the controller's axes are transposed
     * relative to the panel and in its own ~0..1650 x 0..900 space. Measured at
     * edge midpoints (corners read short on this digitiser). Describes the glass
     * relative to the native portrait raster, so it holds for any desktop
     * orientation. */
    gsl3680_config_t cfg = {
        .bus      = bus,
        .dev_addr = BSP_TOUCH_I2C_ADDR,
        .rst_gpio = BSP_TOUCH_RST,
        .int_gpio = BSP_TOUCH_INT,
        .x_max    = BSP_LCD_H_RES,
        .y_max    = BSP_LCD_V_RES,
        .flags    = { .swap_xy = true },
        .cal = {
            .x_raw_at_min =   20,
            .x_raw_at_max =  888,
            .y_raw_at_min = 1644,
            .y_raw_at_max =   31,
        },
    };
    ESP_RETURN_ON_ERROR(gsl3680_new(&cfg, &s_gsl), TAG, "gsl3680");

    BaseType_t ok = xTaskCreatePinnedToCore(touch_task, "touch", 4096, NULL, 6, NULL, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    ESP_LOGI(TAG, "GSL3680 up, reporting as HID touchscreen (%d contacts)", TOUCH_MAX_CONTACTS);
    return ESP_OK;
}
