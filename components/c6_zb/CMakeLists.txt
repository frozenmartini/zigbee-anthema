idf_component_register(SRCS "c6_zb.c"
                       INCLUDE_DIRS "include"
                       REQUIRES espressif__esp-zigbee-lib
                       PRIV_REQUIRES esp_timer)
