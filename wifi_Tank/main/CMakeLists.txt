idf_component_register(SRCS "main.c" "system.c" "stream.c"
                    INCLUDE_DIRS "."
                    REQUIRES
                        src
                        esp_wifi
                        esp_event
                        nvs_flash
                        esp_http_server
                        esp_netif)