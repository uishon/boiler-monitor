#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

void mqtt_init(void);
bool mqtt_is_connected(void);
esp_err_t mqtt_publish_temperatures(const float temperatures_c[2],
                                    const uint64_t sensor_addresses[2],
                                    size_t sensor_count);