idf_component_register(SRCS "sd_card.c"
                    INCLUDE_DIRS "."
                    REQUIRES board_config fatfs sdmmc driver)