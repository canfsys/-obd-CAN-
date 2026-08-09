idf_component_register(SRCS "battery.c"
                    INCLUDE_DIRS "."
                    REQUIRES board_config driver esp_adc)