#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "tusb.h"

/*
 * USB Audio Class 2 speaker -> ES8311 codec -> NS4150 amp.
 *
 * 48 kHz, stereo, 16-bit, asynchronous with a feedback endpoint: the host's
 * sample rate is steered to the codec's I2S clock by TinyUSB's FIFO-count
 * method, so the two clocks cannot drift apart into periodic clicks. The
 * host's volume and mute drive the codec directly (UAC2 feature unit).
 */

/* Interface numbers 2 and 3 of the composite device (see usb_link.c). */
#define AUDIO_ITF_CONTROL   2
#define AUDIO_ITF_STREAMING 3
#define AUDIO_EP_OUT        0x03
#define AUDIO_EP_FEEDBACK   0x83
#define AUDIO_STR_IDX       6

#define AUDIO_SAMPLE_RATE   48000
#define AUDIO_CHANNELS      2
#define AUDIO_BYTES_PER_SMP 2

/* Entity IDs used by AUDIO_SPEAKER_DESCRIPTOR. */
#define AUDIO_ENTITY_INPUT_TERMINAL  0x01
#define AUDIO_ENTITY_FEATURE_UNIT    0x02
#define AUDIO_ENTITY_OUTPUT_TERMINAL 0x03
#define AUDIO_ENTITY_CLOCK           0x04

/* UAC2 stereo speaker with feedback, adapted from TinyUSB's uac2_speaker_fb
 * example (TUD_AUDIO20_SPEAKER_STEREO_FB_DESCRIPTOR). Changes: channels are
 * declared front-left/front-right so the host labels them, and both endpoint
 * intervals are passed in per bus speed.
 *
 * At high speed the data endpoint runs every 1 ms (bInterval 4), not every
 * 125 us microframe. While the JPEG engine decodes, the USB completion
 * interrupt can land late; at a 125 us interval the endpoint is then re-armed
 * for the wrong microframe and that packet is lost - about one per decoded
 * frame, audible as static. At 1 ms there is ~875 us of slack. This needs the
 * patched DWC2 driver (components/espressif__tinyusb/PATCHES.md): stock
 * TinyUSB mis-targets the parity of every packet at any even interval. */
#define AUDIO_SPEAKER_DESC_LEN (TUD_AUDIO20_DESC_IAD_LEN            \
    + TUD_AUDIO20_DESC_STD_AC_LEN                                   \
    + TUD_AUDIO20_DESC_CS_AC_LEN                                    \
    + TUD_AUDIO20_DESC_CLK_SRC_LEN                                  \
    + TUD_AUDIO20_DESC_INPUT_TERM_LEN                               \
    + TUD_AUDIO20_DESC_OUTPUT_TERM_LEN                              \
    + TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(2)                          \
    + TUD_AUDIO20_DESC_STD_AS_LEN                                   \
    + TUD_AUDIO20_DESC_STD_AS_LEN                                   \
    + TUD_AUDIO20_DESC_CS_AS_INT_LEN                                \
    + TUD_AUDIO20_DESC_TYPE_I_FORMAT_LEN                            \
    + TUD_AUDIO20_DESC_STD_AS_ISO_EP_LEN                            \
    + TUD_AUDIO20_DESC_CS_AS_ISO_EP_LEN                             \
    + TUD_AUDIO20_DESC_STD_AS_ISO_FB_EP_LEN)

#define AUDIO_FU_CTRLS (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS | \
                        AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS)
#define AUDIO_STEREO   (AUDIO20_CHANNEL_CONFIG_FRONT_LEFT | AUDIO20_CHANNEL_CONFIG_FRONT_RIGHT)

#define AUDIO_SPEAKER_DESCRIPTOR(_epoutsize, _data_interval, _fb_interval) \
    TUD_AUDIO20_DESC_IAD(AUDIO_ITF_CONTROL, 0x02, 0x00), \
    TUD_AUDIO20_DESC_STD_AC(AUDIO_ITF_CONTROL, 0x00, AUDIO_STR_IDX), \
    TUD_AUDIO20_DESC_CS_AC(0x0200, AUDIO20_FUNC_DESKTOP_SPEAKER, \
        TUD_AUDIO20_DESC_CLK_SRC_LEN + TUD_AUDIO20_DESC_INPUT_TERM_LEN + \
        TUD_AUDIO20_DESC_OUTPUT_TERM_LEN + TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(2), \
        AUDIO20_CS_AS_INTERFACE_CTRL_LATENCY_POS), \
    TUD_AUDIO20_DESC_CLK_SRC(AUDIO_ENTITY_CLOCK, AUDIO20_CLOCK_SOURCE_ATT_INT_PRO_CLK, \
        (AUDIO20_CTRL_R << AUDIO20_CLOCK_SOURCE_CTRL_CLK_FRQ_POS), AUDIO_ENTITY_INPUT_TERMINAL, 0x00), \
    TUD_AUDIO20_DESC_INPUT_TERM(AUDIO_ENTITY_INPUT_TERMINAL, AUDIO_TERM_TYPE_USB_STREAMING, 0x00, \
        AUDIO_ENTITY_CLOCK, AUDIO_CHANNELS, AUDIO_STEREO, 0x00, 0x0000, 0x00), \
    TUD_AUDIO20_DESC_OUTPUT_TERM(AUDIO_ENTITY_OUTPUT_TERMINAL, AUDIO_TERM_TYPE_OUT_DESKTOP_SPEAKER, \
        AUDIO_ENTITY_INPUT_TERMINAL, AUDIO_ENTITY_FEATURE_UNIT, AUDIO_ENTITY_CLOCK, 0x0000, 0x00), \
    TUD_AUDIO20_DESC_FEATURE_UNIT(AUDIO_ENTITY_FEATURE_UNIT, AUDIO_ENTITY_INPUT_TERMINAL, 0x00, \
        AUDIO_FU_CTRLS, AUDIO_FU_CTRLS, AUDIO_FU_CTRLS), \
    /* Alternate 0: no bandwidth. Alternate 1: streaming. */ \
    TUD_AUDIO20_DESC_STD_AS_INT(AUDIO_ITF_STREAMING, 0x00, 0x00, 0x00), \
    TUD_AUDIO20_DESC_STD_AS_INT(AUDIO_ITF_STREAMING, 0x01, 0x02, 0x00), \
    TUD_AUDIO20_DESC_CS_AS_INT(AUDIO_ENTITY_INPUT_TERMINAL, AUDIO20_CTRL_NONE, AUDIO20_FORMAT_TYPE_I, \
        AUDIO20_DATA_FORMAT_TYPE_I_PCM, AUDIO_CHANNELS, AUDIO_STEREO, 0x00), \
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(AUDIO_BYTES_PER_SMP, AUDIO_BYTES_PER_SMP * 8), \
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(AUDIO_EP_OUT, \
        (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_ASYNCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_DATA), \
        _epoutsize, _data_interval), \
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO20_CTRL_NONE, \
        AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, 0x0001), \
    TUD_AUDIO20_DESC_STD_AS_ISO_FB_EP(AUDIO_EP_FEEDBACK, 4, _fb_interval)

/* Brings up I2S and the ES8311 and starts the task that feeds USB audio to it.
 * The amp stays off until the host starts streaming. */
esp_err_t audio_init(i2c_master_bus_handle_t bus);

/* Playback health, appended to the display link's stats reply. */
typedef struct __attribute__((packed)) {
    uint32_t streams;      /* times the host started streaming */
    uint32_t underruns;    /* FIFO ran dry mid-stream (audible gap) */
    uint32_t chunks;       /* 1 ms chunks played */
    uint16_t fifo_min;     /* lowest FIFO level seen since last read, bytes */
    uint16_t fifo_max;     /* highest, bytes; target is 2048 (audio.c FIFO_TARGET) */
    uint32_t rx_packets;   /* isochronous packets received (1000/s expected) */
    uint32_t rx_bytes;     /* audio bytes received (192000/s at 48 kHz stereo 16-bit) */
} audio_stats_t;

/* Copies the counters and resets the min/max window. */
void audio_get_stats(audio_stats_t *out);
