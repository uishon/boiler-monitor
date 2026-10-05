#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "sensors.h"
#include "mqtt.h"
#include "oled.h"

static const char *TAG = "boiler";
static bool s_wifi_connected;
static bool s_mqtt_watchdog_test_pending;
static bool s_mqtt_network_recovery_attempted;
static char s_ip_address[16] = "WAITING";
static uint8_t s_wifi_disconnect_reason;
static const char *BUILD_DATE = __DATE__;
static const char *BUILD_TIME = __TIME__;

#if CONFIG_FREERTOS_UNICORE
#define DISPLAY_TASK_CORE 0
#else
#define DISPLAY_TASK_CORE 1
#endif

#define DISPLAY_RECOVERY_COOLDOWN_MS 250U
#define MQTT_NETWORK_RECOVERY_US (60ULL * 1000000ULL)
#define MQTT_PUBLISH_WATCHDOG_US (5ULL * 60ULL * 1000000ULL)
#define LOG_BUFFER_SIZE 2048U
#define LOG_LINE_SIZE 256U

static vprintf_like_t s_default_log_output;
static portMUX_TYPE s_log_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_log_buffer[LOG_BUFFER_SIZE];
static size_t s_log_buffer_used;

static int log_capture_vprintf(const char *format, va_list arguments)
{
    char line[LOG_LINE_SIZE];
    va_list copy;
    va_copy(copy, arguments);
    int written = vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);

    if (written > 0) {
        size_t length = (size_t)written;
        if (length >= sizeof(line)) {
            length = sizeof(line) - 1U;
        }

        taskENTER_CRITICAL(&s_log_lock);
        size_t discard = s_log_buffer_used + length;
        if (discard > sizeof(s_log_buffer)) {
            discard -= sizeof(s_log_buffer);
            memmove(s_log_buffer, s_log_buffer + discard, s_log_buffer_used - discard);
            s_log_buffer_used -= discard;
        }
        memcpy(s_log_buffer + s_log_buffer_used, line, length);
        s_log_buffer_used += length;
        taskEXIT_CRITICAL(&s_log_lock);
    }

    return s_default_log_output(format, arguments);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event =
            (wifi_event_sta_disconnected_t *)event_data;
        s_wifi_connected = false;
        s_wifi_disconnect_reason = event->reason;
        snprintf(s_ip_address, sizeof(s_ip_address), "WAITING");
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason=%d); reconnecting", event->reason);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        s_wifi_connected = false;
        snprintf(s_ip_address, sizeof(s_ip_address), "WAITING");
        ESP_LOGW(TAG, "Wi-Fi lost its IP address; reconnecting");
        esp_wifi_disconnect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_connected = true;
        snprintf(s_ip_address, sizeof(s_ip_address), IPSTR,
                 IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    } else {
        ESP_LOGI(TAG, "Unhandled Wi-Fi/IP event: base=%s id=%ld",
                 event_base, (long)event_id);
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
    esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {0};
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s",
             CONFIG_WIFI_SSID);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s",
             CONFIG_WIFI_PASSWORD);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
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

static esp_err_t info_handler(httpd_req_t *req)
{
    char resp[960];

    uint64_t sensor0 = sensors_address(0);
    uint64_t sensor1 = sensors_address(1);
    bool mqtt_connected = mqtt_is_connected();
    wifi_ap_record_t ap_info = {0};
    bool wifi_rssi_valid = s_wifi_connected &&
                           esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
    uint32_t uptime_seconds = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    uint32_t mqtt_last_published_seconds =
        (uint32_t)(mqtt_last_published_us() / 1000000ULL);
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *app_version = app_desc->version;
    const char *git_hash = esp_app_get_elf_sha256_str();

    snprintf(resp, sizeof(resp),
             "{\"wifi_connected\":%s,\"mqtt_connected\":%s,\"ip\":\"%s\","
             "\"wifi_rssi_dbm\":%d,\"wifi_rssi_valid\":%s,"
             "\"mqtt_last_published_seconds\":%u,"
             "\"uptime_seconds\":%u,"
             "\"app_version\":\"%s\",\"git_hash\":\"%s\","
             "\"build_date\":\"%s\",\"build_time\":\"%s\","
             "\"temp_c\":[%.2f,%.2f],\"sensor_count\":%u,"
             "\"sensor_addr\":[\"%016llX\",\"%016llX\"],"
             "\"seconds_since_sensor_update\":%u,\"sensor_update_progress\":%u}",
             s_wifi_connected ? "true" : "false",
             mqtt_connected ? "true" : "false",
             s_ip_address,
             (int)ap_info.rssi,
             wifi_rssi_valid ? "true" : "false",
             (unsigned)mqtt_last_published_seconds,
             (unsigned)uptime_seconds,
             app_version,
             git_hash,
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

static esp_err_t logs_handler(httpd_req_t *req)
{
    char response[LOG_BUFFER_SIZE + 1U];

    taskENTER_CRITICAL(&s_log_lock);
    memcpy(response, s_log_buffer, s_log_buffer_used);
    response[s_log_buffer_used] = '\0';
    taskEXIT_CRITICAL(&s_log_lock);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

static esp_err_t help_handler(httpd_req_t *req)
{
    static const char response[] =
        "/status - temperatures\n"
        "/info - diagnostics\n"
        "/logs - captured logs\n"
        "/watchdog - schedule watchdog restart\n"
        "/help - available commands\n";

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

static esp_err_t watchdog_test_handler(httpd_req_t *req)
{
    s_mqtt_watchdog_test_pending = true;
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Wi-Fi watchdog restart scheduled");
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

        httpd_uri_t info_uri = {
            .uri       = "/info",
            .method    = HTTP_GET,
            .handler   = info_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &info_uri);

        httpd_uri_t logs_uri = {
            .uri       = "/logs",
            .method    = HTTP_GET,
            .handler   = logs_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &logs_uri);

        httpd_uri_t help_uri = {
            .uri       = "/help",
            .method    = HTTP_GET,
            .handler   = help_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &help_uri);

        httpd_uri_t watchdog_test_uri = {
            .uri       = "/watchdog",
            .method    = HTTP_GET,
            .handler   = watchdog_test_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &watchdog_test_uri);
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
        wifi_ap_record_t ap_info = {0};
        esp_err_t wifi_rssi_err = ESP_ERR_WIFI_NOT_CONNECT;
        if (s_wifi_connected) {
            wifi_rssi_err = esp_wifi_sta_get_ap_info(&ap_info);
        }
        bool wifi_rssi_valid = s_wifi_connected && wifi_rssi_err == ESP_OK;
        if (s_wifi_connected && wifi_rssi_err != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi RSSI lookup failed: %s",
                     esp_err_to_name(wifi_rssi_err));
        }
        char wifi_display[sizeof(s_ip_address) + 1U];
        if (!s_wifi_connected)
        {
            snprintf(wifi_display, sizeof(wifi_display), "DISCONNECTED %u",
                     (unsigned)s_wifi_disconnect_reason);
        }
        else
        {
            snprintf(wifi_display, sizeof(wifi_display), "%s", s_ip_address);
        }

        esp_err_t err = oled_update(g_temp_c, s_wifi_connected,
                                    ap_info.rssi, wifi_rssi_valid, mqtt_connected,
                                    sensor_addresses,
                                    wifi_display,
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

static void mqtt_publish_watchdog_check(void)
{
    int64_t last_published_us = mqtt_last_published_us();
    int64_t publish_age_us = esp_timer_get_time() - last_published_us;
    bool mqtt_publish_overdue = publish_age_us >= MQTT_PUBLISH_WATCHDOG_US;

    if (publish_age_us < MQTT_NETWORK_RECOVERY_US) {
        s_mqtt_network_recovery_attempted = false;
    } else if (!s_mqtt_network_recovery_attempted && s_wifi_connected) {
        s_mqtt_network_recovery_attempted = true;
        ESP_LOGW(TAG, "No MQTT publish acknowledgement for 60 seconds; reconnecting Wi-Fi");
        esp_wifi_disconnect();
    }

    if (s_mqtt_watchdog_test_pending || mqtt_publish_overdue) {
        ESP_LOGE(TAG, "MQTT publish watchdog restarting device");
        esp_restart();
    }
}

void app_main(void)
{
    s_default_log_output = esp_log_set_vprintf(log_capture_vprintf);
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
        mqtt_publish_watchdog_check();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
