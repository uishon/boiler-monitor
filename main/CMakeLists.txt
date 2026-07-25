idf_component_register(SRCS "sensors.c" "oled.c" "boiler_monitor.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_event esp_http_server esp_lcd esp_netif
                       esp_wifi nvs_flash espressif__ds18b20
                       espressif__onewire_bus)
