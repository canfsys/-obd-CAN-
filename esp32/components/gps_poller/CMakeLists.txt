idf_component_register(SRCS "gps_poller.c"
                    INCLUDE_DIRS "."
    REQUIRES board_config can_parser driver esp_timer)