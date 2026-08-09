idf_component_register(SRCS "power_manager.c"
                    INCLUDE_DIRS "."
    REQUIRES board_config spi_bus sit1145 esp_timer)