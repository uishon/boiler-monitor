#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define MQTT_SENSOR_VALUE_COUNT 2U

void mqtt_init(void);
bool mqtt_is_connected(void);
esp_err_t mqtt_publish_temperatures(const float temperatures_c[MQTT_SENSOR_VALUE_COUNT],
                                    const uint64_t sensor_addresses[MQTT_SENSOR_VALUE_COUNT],
                                    size_t sensor_count);