idf_component_register(SRCS "audio_out.c" "mic.c" "led.c" "main.c" 
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio esp_driver_i2s led_strip)
