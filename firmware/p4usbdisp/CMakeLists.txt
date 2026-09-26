cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# USB Audio Class 2 speaker (main/audio.c). esp_tinyusb 1.x has no Kconfig for
# the audio class, but TinyUSB only defaults these when undefined, so they are
# set build-wide: TinyUSB itself, esp_tinyusb and main must all agree.
#   EP_OUT_SZ_MAX: TUD_AUDIO_EP_SIZE for 48 kHz, 2 ch, 16-bit - 196 at full speed
#                  (28 at high speed); must cover whichever is larger.
#   EP_OUT_SW_BUF_SZ: ~21 ms of audio. The FIFO-count feedback aims for half of it.
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_AUDIO=1" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_AUDIO_ENABLE_EP_OUT=1" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP=1" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX=196" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ=4096" APPEND)

project(p4usbdisp)
