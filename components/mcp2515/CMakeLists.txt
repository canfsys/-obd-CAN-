idf_component_register(SRCS "mcp2515.c"
                    INCLUDE_DIRS "."
                    REQUIRES board_config spi_bus)