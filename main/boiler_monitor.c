#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "sensors.h"
#include "mqtt.h"
#include "oled.h"

static const char *TAG = "boiler";
static bool s_wifi_connected;
static char s_ip_address[16] = "WAITING";
static const char *BUILD_DATE = __DATE__;
static const char *BUILD_TIME = __TIME__;

#ifndef APP_GIT_HASH
#define APP_GIT_HASH "unknown"
#endif

#if CONFIG_FREERTOS_UNICORE
#define DISPLAY_TASK_CORE 0
#else
#define DISPLAY_TASK_CORE 1
#endif

#define DISPLAY_RECOVERY_COOLDOWN_MS 250U

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        snprintf(s_ip_address, sizeof(s_ip_address), "WAITING");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_connected = true;
        snprintf(s_ip_address, sizeof(s_ip_address), IPSTR,
                 IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void wifi_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {0};
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s",
             CONFIG_WIFI_SSID);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s",
             CONFIG_WIFI_PASSWORD);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char resp[64];
    snprintf(resp, sizeof(resp),
             "%.2fC %.2fC",
             g_temp_c[0], g_temp_c[1]);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t diag_handler(httpd_req_t *req)
{
    char resp[960];

    uint64_t sensor0 = sensors_address(0);
    uint64_t sensor1 = sensors_address(1);
    bool mqtt_connected = mqtt_is_connected();
    uint32_t uptime_seconds = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    const esp_app_desc_t *app_desc = esp_app_get_description();

    const char *app_version = app_desc->version;

    snprintf(resp, sizeof(resp),
             "{\"wifi_connected\":%s,\"mqtt_connected\":%s,\"ip\":\"%s\","
             "\"uptime_seconds\":%u,"
             "\"app_version\":\"%s\",\"git_hash\":\"%s\","
             "\"build_date\":\"%s\",\"build_time\":\"%s\","
             "\"temp_c\":[%.2f,%.2f],\"sensor_count\":%u,"
             "\"sensor_addr\":[\"%016llX\",\"%016llX\"],"
             "\"seconds_since_sensor_update\":%u,\"sensor_update_progress\":%u}",
             s_wifi_connected ? "true" : "false",
             mqtt_connected ? "true" : "false",
             s_ip_address,
             (unsigned)uptime_seconds,
             app_version,
             APP_GIT_HASH,
             BUILD_DATE,
             BUILD_TIME,
             g_temp_c[0], g_temp_c[1],
             (unsigned)sensors_count(),
             (unsigned long long)sensor0,
             (unsigned long long)sensor1,
             (unsigned)sensors_seconds_since_update(),
             (unsigned)sensors_update_progress_percent());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static void http_server_init(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t status_uri = {
            .uri       = "/status",
            .method    = HTTP_GET,
            .handler   = status_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &status_uri);

        httpd_uri_t diag_uri = {
            .uri       = "/diag",
            .method    = HTTP_GET,
            .handler   = diag_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &diag_uri);
    }
}

static void display_task(void *arg)
{
    uint8_t consecutive_failures = 0;

    while (true) {
        uint32_t seconds_since_update = sensors_seconds_since_update();
        uint8_t update_progress_percent = sensors_update_progress_percent();
        bool mqtt_connected = mqtt_is_connected();
        uint64_t sensor_addresses[2] = {
            sensors_address(0),
            sensors_address(1),
        };

        esp_err_t err = oled_update(g_temp_c, s_wifi_connected, mqtt_connected,
                                    sensor_addresses,
                                    s_ip_address,
                                    seconds_since_update, update_progress_percent);
        if (err != ESP_OK) {
            consecutive_failures++;
            ESP_LOGW(TAG, "OLED update failed: %s", esp_err_to_name(err));
            bool should_recover_now = (err == ESP_ERR_INVALID_RESPONSE) ||
                                      (consecutive_failures >= 3);
            if (should_recover_now) {
                esp_err_t recover_err = oled_recover();
                if (recover_err == ESP_OK) {
                    ESP_LOGW(TAG, "OLED recovered after %u failures", consecutive_failures);
                    consecutive_failures = 0;
                } else {
                    ESP_LOGW(TAG, "OLED recovery failed: %s", esp_err_to_name(recover_err));
                    vTaskDelay(pdMS_TO_TICKS(3000));
                }

                // Brief cooldown helps avoid immediate re-contention on the I2C lines.
                vTaskDelay(pdMS_TO_TICKS(DISPLAY_RECOVERY_COOLDOWN_MS));
            }
        } else {
            consecutive_failures = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    nvs_flash_init();
    wifi_init();
    mqtt_init();
    http_server_init();
    sensors_init();
    ESP_ERROR_CHECK(oled_init());
    xTaskCreate(sensors_task, "sensors_task", 4096, NULL, 5, NULL);
    xTaskCreatePinnedToCore(display_task, "display_task", 4096, NULL, 3, NULL,
                            DISPLAY_TASK_CORE);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
