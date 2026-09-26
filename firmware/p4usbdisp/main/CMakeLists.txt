idf_component_register(
    SRCS "main.c" "bsp_display.c" "usb_link.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_ledc esp_driver_jpeg esp_hw_support esp_mm esp_timer)
