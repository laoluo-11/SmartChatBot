idf_component_register(
    SRCS
        "oled.c"
        "audio_out.c"
        "mic.c"
        "led.c"
        "button.c"
        "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_lcd led_strip
)
