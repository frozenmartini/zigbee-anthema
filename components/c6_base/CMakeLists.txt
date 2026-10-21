idf_component_register(SRCS "c6_base.c" "c6_wifi.c" "c6_led.c" "c6_auth.c" "c6_button.c" "c6_discovery.c"
                            "improv.c" "portal.c" "dns.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash app_update
                                     esp_driver_gpio esp_timer esp_coex
                                     mbedtls esp_driver_usb_serial_jtag lwip)
