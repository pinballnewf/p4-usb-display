/*
 * Audio: USB Audio Class 2 speaker -> I2S0 -> ES8311 -> NS4150 amp.
 *
 * The ES8311 is a mono codec (one DAC, one speaker via the NS4150): the host
 * sees a stereo speaker, and the two channels are mixed down here.
 *
 * Codec bring-up follows the author's earlier, proven firmware for this board, with two
 * changes: playback only for now (the board's single analog mic can follow as a
 * UAC2 input), and the amp's enable line (PA_CTRL) is driven here rather than by
 * esp_codec_dev, so the amp only runs while the host is actually streaming -
 * left on, it hisses.
 *
 * Rate matching: the codec's I2S clock is derived from the P4's own PLL, not
 * from USB, so the two drift. The endpoint is asynchronous with a feedback
 * endpoint, and TinyUSB's FIFO-count method steers the host's rate to hold the
 * receive FIFO at its target level. This task drains the FIFO in 1 ms chunks
 * into blocking I2S writes, which is what makes its level track the I2S clock.
 */

#include "audio.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "es8311_codec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp_pins.h"

static const char *TAG = "audio";

#define FRAME_BYTES  (AUDIO_CHANNELS * AUDIO_BYTES_PER_SMP)
#define CHUNK_FRAMES (AUDIO_SAMPLE_RATE / 1000) /* 1 ms */
#define CHUNK_BYTES  (CHUNK_FRAMES * FRAME_BYTES)
#define FIFO_BYTES   CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ
/* Feedback target: ~10.7 ms of audio. Well below FIFO_BYTES on purpose - the
 * FIFO-count controller is proportional on a slow average and overshoots by
 * about another 2 KB at the start of every stream; with the target at half of
 * a 4 KB FIFO that overshoot filled it and dropped samples. */
#define FIFO_TARGET  2048

/* I2S DMA ring: 4 x 240 frames = 20 ms. Filled with silence at stream start so
 * every write after that blocks at the real sample rate (see audio_task). */
#define I2S_DMA_DESC   4
#define I2S_DMA_FRAMES 240
#define I2S_RING_BYTES (I2S_DMA_DESC * I2S_DMA_FRAMES * FRAME_BYTES)

/* Host volume range: -50..0 dB in 1 dB steps, in the UAC2 1/256 dB unit. */
#define VOL_MIN_DB256 (-50 * 256)
#define VOL_MAX_DB256 0
#define VOL_RES_DB256 256

static i2s_chan_handle_t      s_tx;
static esp_codec_dev_handle_t s_dev;

/* Written by TinyUSB's task (control requests), applied by the audio task so
 * the codec's I2C writes never run inside a USB callback. Index 0 is the master
 * channel, 1 and 2 are left and right. */
static volatile bool    s_streaming;
static volatile bool    s_dirty = true;
static volatile int16_t s_vol[AUDIO_CHANNELS + 1] = { -10 * 256, 0, 0 };
static volatile bool    s_mute[AUDIO_CHANNELS + 1];

static audio_stats_t s_stats = { .fifo_min = UINT16_MAX };

void audio_get_stats(audio_stats_t *out)
{
    *out = s_stats;
    s_stats.fifo_min = UINT16_MAX;
    s_stats.fifo_max = 0;
}

static void pa_enable(bool on)
{
    gpio_set_level(BSP_PA_CTRL, on); /* NS4150 enables on a high level */
}

static void apply_volume(void)
{
    s_dirty = false;
    int db256 = s_vol[0] + (s_vol[1] + s_vol[2]) / 2;
    if (db256 < VOL_MIN_DB256) db256 = VOL_MIN_DB256;
    if (db256 > VOL_MAX_DB256) db256 = VOL_MAX_DB256;
    /* esp_codec_dev takes 0..100; map the host's -50..0 dB linearly onto it. */
    int percent = 100 + (db256 * 2) / 256;
    bool mute = s_mute[0] || (s_mute[1] && s_mute[2]);

    esp_codec_dev_set_out_vol(s_dev, percent);
    esp_codec_dev_set_out_mute(s_dev, mute);
    ESP_LOGI(TAG, "volume %d dB (%d%%)%s", db256 / 256, percent, mute ? ", muted" : "");
}

static void audio_task(void *arg)
{
    static int16_t buf[CHUNK_BYTES / 2];
    bool running = false;
    bool amp_on = false;

    for (;;) {
        if (s_dirty) {
            apply_volume();
        }

        if (!s_streaming) {
            if (amp_on) {
                pa_enable(false);
                amp_on = false;
                ESP_LOGI(TAG, "stream stopped, amp off");
            }
            running = false;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (!running) {
            /* Prefill to the feedback target (half the FIFO) before draining,
             * so the rate loop starts from where it wants to be. */
            if (tud_audio_available() < FIFO_TARGET) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
            /* Fill the I2S DMA ring with silence first. Writes into an empty
             * ring return immediately, so without this the loop drained the
             * USB FIFO far faster than real time, underran, re-prefilled and
             * did it again - five or six gaps at the start of every stream.
             * With the ring full, every write blocks for exactly its own
             * duration from the first chunk on. In steady state the ring is
             * full anyway, so this adds no latency. It also clocks silence
             * through the codec before the amp comes up. */
            memset(buf, 0, sizeof(buf));
            for (int b = 0; b < I2S_RING_BYTES; b += sizeof(buf)) {
                esp_codec_dev_write(s_dev, buf, sizeof(buf));
            }
            running = true;
            if (!amp_on) {
                pa_enable(true);
                amp_on = true;
                s_stats.streams++;
                ESP_LOGI(TAG, "stream started, amp on");
            }
        }

        uint16_t level = tud_audio_available();
        if (level < s_stats.fifo_min) s_stats.fifo_min = level;
        if (level > s_stats.fifo_max) s_stats.fifo_max = level;

        uint16_t n = tud_audio_read(buf, sizeof(buf));
        s_stats.chunks++;
        if (n < sizeof(buf)) {
            /* Underrun: pad with silence and keep the I2S pacing going. The
             * feedback loop sees the low FIFO and speeds the host up; pausing
             * to re-prefill would let the ring drain and cascade into more. */
            memset((uint8_t *)buf + n, 0, sizeof(buf) - n);
            s_stats.underruns++;
        }
        /* The ES8311 has a single DAC fed from the left slot, driving one
         * speaker: fold both channels into it so nothing panned right is lost. */
        for (int i = 0; i < CHUNK_FRAMES; i++) {
            int16_t m = (int16_t)(((int32_t)buf[2 * i] + buf[2 * i + 1]) / 2);
            buf[2 * i] = buf[2 * i + 1] = m;
        }
        esp_codec_dev_write(s_dev, buf, sizeof(buf)); /* blocks: paces the loop */
    }
}

esp_err_t audio_init(i2c_master_bus_handle_t bus)
{
    gpio_config_t pa = {
        .pin_bit_mask = 1ULL << BSP_PA_CTRL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&pa), TAG, "PA gpio");
    pa_enable(false);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BSP_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; /* emit silence rather than the last buffer */
    chan_cfg.dma_desc_num = I2S_DMA_DESC;
    chan_cfg.dma_frame_num = I2S_DMA_FRAMES;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_BCLK,
            .ws   = BSP_I2S_WS,
            .dout = BSP_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s tx");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = BSP_I2S_PORT, .tx_handle = s_tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(data_if, ESP_FAIL, TAG, "i2s data if");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = BSP_I2C_PORT,
        .addr       = ES8311_CODEC_DEFAULT_ADDR, /* 8-bit form of 0x18 */
        .bus_handle = bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "i2c ctrl if");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if     = ctrl_if,
        .gpio_if     = audio_codec_new_gpio(),
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin      = -1, /* driven by audio_task, see header comment */
        .master_mode = false, /* the P4 is I2S master */
        .use_mclk    = true,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "es8311 (is it at 0x18?)");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if  = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_dev, ESP_FAIL, TAG, "codec dev");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = AUDIO_BYTES_PER_SMP * 8,
        .channel         = AUDIO_CHANNELS,
        .sample_rate     = AUDIO_SAMPLE_RATE,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_dev, &fs) == 0, ESP_FAIL, TAG, "codec open");

    BaseType_t ok = xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 7, NULL, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    ESP_LOGI(TAG, "ES8311 up: UAC2 speaker, %d Hz stereo 16-bit", AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

/* ---- UAC2 class callbacks (TinyUSB task context) ---- */

static bool clock_get(uint8_t rhport, tusb_control_request_t const *req)
{
    uint8_t sel = TU_U16_HIGH(req->wValue);
    if (sel == AUDIO20_CS_CTRL_SAM_FREQ && req->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_4_t cur = { .bCur = tu_htole32(AUDIO_SAMPLE_RATE) };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &cur, sizeof(cur));
    }
    if (sel == AUDIO20_CS_CTRL_SAM_FREQ && req->bRequest == AUDIO20_CS_REQ_RANGE) {
        audio20_control_range_4_n_t(1) range = {
            .wNumSubRanges = tu_htole16(1),
            .subrange[0] = { tu_htole32(AUDIO_SAMPLE_RATE), tu_htole32(AUDIO_SAMPLE_RATE), 0 },
        };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &range, sizeof(range));
    }
    if (sel == AUDIO20_CS_CTRL_CLK_VALID && req->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_1_t valid = { .bCur = 1 };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &valid, sizeof(valid));
    }
    return false;
}

static bool clock_set(tusb_control_request_t const *req, uint8_t const *buf)
{
    /* Only one rate exists; accept a host that sets it anyway. */
    return TU_U16_HIGH(req->wValue) == AUDIO20_CS_CTRL_SAM_FREQ &&
           req->bRequest == AUDIO20_CS_REQ_CUR &&
           req->wLength == sizeof(audio20_control_cur_4_t) &&
           ((audio20_control_cur_4_t const *)buf)->bCur == AUDIO_SAMPLE_RATE;
}

static bool feature_get(uint8_t rhport, tusb_control_request_t const *req)
{
    uint8_t sel = TU_U16_HIGH(req->wValue);
    uint8_t ch = TU_U16_LOW(req->wValue);
    TU_VERIFY(ch <= AUDIO_CHANNELS); /* host-controlled index */

    if (sel == AUDIO20_FU_CTRL_MUTE && req->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_1_t m = { .bCur = s_mute[ch] };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &m, sizeof(m));
    }
    if (sel == AUDIO20_FU_CTRL_VOLUME && req->bRequest == AUDIO20_CS_REQ_RANGE) {
        audio20_control_range_2_n_t(1) r = {
            .wNumSubRanges = tu_htole16(1),
            .subrange[0] = { tu_htole16(VOL_MIN_DB256), tu_htole16(VOL_MAX_DB256), tu_htole16(VOL_RES_DB256) },
        };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &r, sizeof(r));
    }
    if (sel == AUDIO20_FU_CTRL_VOLUME && req->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_2_t v = { .bCur = tu_htole16(s_vol[ch]) };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &v, sizeof(v));
    }
    return false;
}

static bool feature_set(tusb_control_request_t const *req, uint8_t const *buf)
{
    uint8_t sel = TU_U16_HIGH(req->wValue);
    uint8_t ch = TU_U16_LOW(req->wValue);
    TU_VERIFY(ch <= AUDIO_CHANNELS && req->bRequest == AUDIO20_CS_REQ_CUR);

    if (sel == AUDIO20_FU_CTRL_MUTE && req->wLength == sizeof(audio20_control_cur_1_t)) {
        s_mute[ch] = ((audio20_control_cur_1_t const *)buf)->bCur;
        s_dirty = true;
        return true;
    }
    if (sel == AUDIO20_FU_CTRL_VOLUME && req->wLength == sizeof(audio20_control_cur_2_t)) {
        s_vol[ch] = ((audio20_control_cur_2_t const *)buf)->bCur;
        s_dirty = true;
        return true;
    }
    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *req)
{
    switch (TU_U16_HIGH(req->wIndex)) {
    case AUDIO_ENTITY_CLOCK:        return clock_get(rhport, req);
    case AUDIO_ENTITY_FEATURE_UNIT: return feature_get(rhport, req);
    default:                        return false;
    }
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *req, uint8_t *buf)
{
    (void)rhport;
    switch (TU_U16_HIGH(req->wIndex)) {
    case AUDIO_ENTITY_CLOCK:        return clock_set(req, buf);
    case AUDIO_ENTITY_FEATURE_UNIT: return feature_set(req, buf);
    default:                        return false;
    }
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *req)
{
    (void)rhport;
    if (tu_u16_low(req->wIndex) == AUDIO_ITF_STREAMING) {
        s_streaming = tu_u16_low(req->wValue) != 0;
    }
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *req)
{
    (void)rhport;
    if (tu_u16_low(req->wIndex) == AUDIO_ITF_STREAMING && tu_u16_low(req->wValue) == 0) {
        s_streaming = false;
    }
    return true;
}

/* ISR context: every isochronous OUT packet that made it into the FIFO.
 * In IRAM with the rest of the USB interrupt path (see linker.lf). */
IRAM_ATTR bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id,
                           uint8_t ep_out, uint8_t cur_alt_setting)
{
    s_stats.rx_packets++;
    s_stats.rx_bytes += n_bytes_received;
    return true;
}

void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t *p)
{
    (void)func_id;
    (void)alt_itf;
    p->method = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
    p->sample_freq = AUDIO_SAMPLE_RATE;
    p->fifo_count.fifo_threshold = FIFO_TARGET;
}
