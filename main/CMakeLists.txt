idf_component_register(SRCS "sensors.c" "oled.c" "boiler_monitor.c" "mqtt.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_app_format esp_event esp_http_server esp_netif mqtt u8g2
                       esp_wifi nvs_flash espressif__ds18b20
                       espressif__onewire_bus
                       PRIV_REQUIRES esp_driver_gpio)
