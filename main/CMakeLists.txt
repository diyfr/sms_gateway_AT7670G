idf_component_register(SRCS "main.c"
                            "rest_server.c"
                            "board.c"
                            "power.c"
                            "contacts.c"
                            "wifi_prov.c"
                       PRIV_REQUIRES esp_http_server nvs_flash 
                       REQUIRES esp_driver_gpio driver esp_modem esp_wifi esp_netif esp_system esp_app_format esp_adc freertos log
                       INCLUDE_DIRS ".")
