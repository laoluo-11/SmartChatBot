idf_component_register(SRCS "audio_out.c" "mic.c" "led.c" "oled.c" "button.c" "state_machine.c" "wifi.c" "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio esp_driver_i2s led_strip esp_lcd esp_driver_i2c esp_wifi esp_netif esp_http_server nvs_flash esp_event)
