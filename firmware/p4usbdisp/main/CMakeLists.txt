idf_component_register(
    SRCS "main.c" "bsp_display.c" "usb_link.c" "touch.c" "audio.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_ledc esp_driver_jpeg esp_hw_support esp_mm esp_timer esp_driver_i2c esp_driver_i2s esp_driver_gpio gsl3680)
