execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE APP_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if(NOT APP_GIT_HASH)
    set(APP_GIT_HASH "unknown")
endif()

idf_component_register(SRCS "sensors.c" "oled.c" "boiler_monitor.c" "mqtt.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_app_format esp_event esp_http_server esp_netif mqtt u8g2
                       esp_wifi nvs_flash espressif__ds18b20
                       espressif__onewire_bus)

target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_GIT_HASH="${APP_GIT_HASH}")
