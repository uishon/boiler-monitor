#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t oled_init(void);
esp_err_t oled_update(const float temperatures_c[2], bool wifi_connected,
                      const char *ip_address);
