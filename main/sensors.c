#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "onewire_bus.h"
#include "onewire_device.h"
#include "ds18b20.h"

#include "mqtt.h"

#define ONEWIRE_PIN GPIO_NUM_4
#define SENSOR_UPDATE_PERIOD_MS 5000U
#define MAX_SENSOR_COUNT 8U
static const char *TAG = "sensors";

// Global temperature arrays
float g_temp_c[MQTT_SENSOR_VALUE_COUNT];
float g_temp_f[MQTT_SENSOR_VALUE_COUNT];

// Store bus + devices
static onewire_bus_handle_t g_bus = NULL;
static ds18b20_device_handle_t g_devices[MAX_SENSOR_COUNT];
static uint64_t g_device_addresses[MAX_SENSOR_COUNT];
static size_t g_device_count = 0;
static volatile TickType_t g_last_update_tick;

size_t sensors_count(void)
{
    return g_device_count;
}

uint64_t sensors_address(size_t index)
{
    if (index >= g_device_count) {
        return 0;
    }
    return g_device_addresses[index];
}

uint32_t sensors_seconds_since_update(void)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t elapsed_ticks = now - g_last_update_tick;
    return (uint32_t)(elapsed_ticks / pdMS_TO_TICKS(1000));
}

uint8_t sensors_update_progress_percent(void)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t elapsed_ticks = now - g_last_update_tick;
    TickType_t period_ticks = pdMS_TO_TICKS(SENSOR_UPDATE_PERIOD_MS);

    if (period_ticks == 0) {
        return 0;
    }

    if (elapsed_ticks >= period_ticks) {
        return 100;
    }

    return (uint8_t)((elapsed_ticks * 100U) / period_ticks);
}

void sensors_init(void)
{
    ESP_LOGI(TAG, "Initializing OneWire bus on GPIO %d", ONEWIRE_PIN);

    // Configure bus
    onewire_bus_config_t bus_cfg = {
        .bus_gpio_num = ONEWIRE_PIN,
   };
    onewire_bus_rmt_config_t rmt_cfg = {
        .max_rx_bytes = 10,
    };

    // Create bus
    ESP_ERROR_CHECK(onewire_new_bus_rmt(&bus_cfg, &rmt_cfg, &g_bus));

    // Create iterator
    onewire_device_iter_handle_t iter = NULL;
    ESP_ERROR_CHECK(onewire_new_device_iter(g_bus, &iter));

    // Enumerate devices
    onewire_device_t dev;
    g_device_count = 0;

    while (onewire_device_iter_get_next(iter, &dev) == ESP_OK) {
        ESP_LOGI(TAG, "Found device ROM: %016llX", dev.address);

        // Define default configuration settings
        ds18b20_config_t ds_cfg = {}; 

        // Create DS18B20 device handle using the config struct
        ds18b20_device_handle_t ds;
        ESP_ERROR_CHECK(ds18b20_new_device_from_enumeration(&dev, &ds_cfg, &ds));
        g_devices[g_device_count] = ds;
        g_device_addresses[g_device_count] = dev.address;
        g_device_count++;

        if (g_device_count >= MAX_SENSOR_COUNT) break;
    }

    ESP_LOGI(TAG, "Total DS18B20 sensors: %u", g_device_count);

    ESP_ERROR_CHECK(onewire_del_device_iter(iter));
    g_last_update_tick = xTaskGetTickCount();
}

void sensors_task(void *arg)
{
    while (1) {

        if (g_device_count == 0) {
            ESP_LOGW(TAG, "No sensors found");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // Trigger conversion for all sensors
        ds18b20_trigger_temperature_conversion_for_all(g_bus);

        // Read each sensor
        for (size_t i = 0; i < g_device_count && i < MQTT_SENSOR_VALUE_COUNT; i++) {
            float temp_c = 0.0f;

            ESP_LOGI(TAG, "Reading sensor %u", (unsigned)i);

            esp_err_t err = ds18b20_get_temperature(g_devices[i], &temp_c);
            if (err == ESP_OK) {
                g_temp_c[i] = temp_c;
                g_temp_f[i] = temp_c * 9.0f / 5.0f + 32.0f;

                ESP_LOGI(TAG, "Sensor %u: %.2f°C / %.2f°F",
                         (unsigned)i, g_temp_c[i], g_temp_f[i]);
            } else {
                ESP_LOGE(TAG, "Failed reading sensor %u: %s",
                         (unsigned)i, esp_err_to_name(err));
            }
        }

        g_last_update_tick = xTaskGetTickCount();

        size_t publish_count = g_device_count;
        size_t max_publish_count = MQTT_SENSOR_VALUE_COUNT;
        if (publish_count > max_publish_count) {
            publish_count = max_publish_count;
        }

        esp_err_t publish_err = mqtt_publish_temperatures(g_temp_c, g_device_addresses,
                                                           publish_count);
        if (publish_err != ESP_OK && publish_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "MQTT publish failed: %s", esp_err_to_name(publish_err));
        }

        vTaskDelay(pdMS_TO_TICKS(SENSOR_UPDATE_PERIOD_MS));
    }
}
