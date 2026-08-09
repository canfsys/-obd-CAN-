idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
    REQUIRES board_config spi_bus sit1145 mcp2515 twai sd_card modem_4g ds18b20 adc can_buffer sd_logger can_parser power_manager gps_poller)