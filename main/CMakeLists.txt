idf_component_register(SRCS "main.c" "http_server.c" "rtc.c" "sd_card.c" "co2_sensor_task.c"
                    INCLUDE_DIRS ".")

add_compile_options(-Wno-format-truncation)