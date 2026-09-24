#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_mac.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"

#include "mqtt.h"

#ifndef CONFIG_MQTT_BROKER_URI
#define CONFIG_MQTT_BROKER_URI "mqtt://broker.hivemq.com"
#endif

#ifndef CONFIG_MQTT_TOPIC
#define CONFIG_MQTT_TOPIC "boiler-monitor/temperature"
#endif

#ifndef CONFIG_MQTT_USERNAME
#define CONFIG_MQTT_USERNAME ""
#endif

#ifndef CONFIG_MQTT_PASSWORD
#define CONFIG_MQTT_PASSWORD ""
#endif

static const char *TAG = "mqtt";
static esp_mqtt_client_handle_t s_client;
static bool s_connected;
static int64_t s_last_published_us;
static char s_client_id[64];
static char s_topic[128];

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "MQTT connected");
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_PUBLISHED:
        s_last_published_us = esp_timer_get_time();
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT error");
        break;
    default:
        (void)event;
        break;
    }
}

void mqtt_init(void)
{
    uint8_t mac[6];
    s_last_published_us = esp_timer_get_time();
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(s_client_id, sizeof(s_client_id), "%s-%02X%02X%02X",
             CONFIG_MQTT_CLIENT_ID, mac[3], mac[4], mac[5]);
    snprintf(s_topic, sizeof(s_topic), "%s/%02X%02X%02X",
             CONFIG_MQTT_TOPIC, mac[3], mac[4], mac[5]);

    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_MQTT_BROKER_URI,
        .credentials.username = CONFIG_MQTT_USERNAME,
        .credentials.authentication.password = CONFIG_MQTT_PASSWORD,
        .credentials.client_id = s_client_id,
    };

    ESP_LOGI(TAG, "Connecting with client ID %s", s_client_id);
    s_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));
}

bool mqtt_is_connected(void)
{
    return s_connected;
}

int64_t mqtt_last_published_us(void)
{
    return s_last_published_us;
}

esp_err_t mqtt_publish_temperatures(const float temperatures_c[MQTT_SENSOR_VALUE_COUNT],
                                    const uint64_t sensor_addresses[MQTT_SENSOR_VALUE_COUNT],
                                    size_t sensor_count)
{
    if (s_client == NULL || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    char payload[192];
    size_t used = 0;
    int written = snprintf(payload + used, sizeof(payload) - used, "{");
    if (written < 0 || (size_t)written >= sizeof(payload) - used) {
        return ESP_ERR_NO_MEM;
    }
    used += (size_t)written;

    bool first = true;
    for (size_t i = 0; i < sensor_count && i < MQTT_SENSOR_VALUE_COUNT; i++) {
        if (sensor_addresses[i] == 0U) {
            continue;
        }

        written = snprintf(payload + used, sizeof(payload) - used,
                           "%s\"%016llX\":%.2f",
                           first ? "" : ",",
                           (unsigned long long)sensor_addresses[i],
                           temperatures_c[i]);
        if (written < 0 || (size_t)written >= sizeof(payload) - used) {
            return ESP_ERR_NO_MEM;
        }
        used += (size_t)written;
        first = false;
    }

    written = snprintf(payload + used, sizeof(payload) - used, "}");
    if (written < 0 || (size_t)written >= sizeof(payload) - used) {
        return ESP_ERR_NO_MEM;
    }

    int msg_id = esp_mqtt_client_publish(s_client, s_topic, payload, 0, 1, 0);
    if (msg_id < 0) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Published %s to %s", payload, s_topic);
    return ESP_OK;
}