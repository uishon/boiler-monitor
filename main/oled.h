#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t oled_init(void);
esp_err_t oled_recover(void);
esp_err_t oled_update(const float temperatures_c[2], bool wifi_connected,
                      bool mqtt_connected,
                      const uint64_t sensor_addresses[2],
                      const char *ip_address,
                      uint32_t seconds_since_update,
                      uint8_t update_progress_percent);
