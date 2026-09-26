cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# USB Audio Class 2 speaker (main/audio.c). esp_tinyusb 1.x has no Kconfig for
# the audio class, but TinyUSB only defaults these when undefined, so they are
# set build-wide: TinyUSB itself, esp_tinyusb and main must all agree.
#   EP_OUT_SZ_MAX: TUD_AUDIO_EP_SIZE for 48 kHz, 2 ch, 16-bit at a 1 ms interval
#                  = 196 (used at both speeds; see main/audio.h).
#   EP_OUT_SW_BUF_SZ: ~43 ms of audio. The feedback targets 2 KB (~10.7 ms, set
#                  in audio.c); the rest is headroom for the start-of-stream
#                  overshoot, which peaked at a full 4 KB FIFO.
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_AUDIO=1" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_AUDIO_ENABLE_EP_OUT=1" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP=1" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX=196" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ=8192" APPEND)

project(p4usbdisp)
