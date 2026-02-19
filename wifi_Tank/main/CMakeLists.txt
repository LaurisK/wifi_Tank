idf_component_register(SRCS "main.c" "system.c" "stream.c" "overlay.c" "provisioning.c"
                    INCLUDE_DIRS "."
                    REQUIRES
                        src
                        esp_wifi
                        esp_event
                        nvs_flash
                        esp_http_server
                        esp_netif
                        esp_timer
                        json
                        espressif__mdns
                    EMBED_TXTFILES
                        "${PROJECT_DIR}/overlay_demo.html"
                        "${PROJECT_DIR}/provisioning.html")