#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "u8g2.h"
#include "u8x8.h"
#include "esp32_hw_i2c.h"

#include "oled.h"

#define OLED_I2C_PORT I2C_NUM_0
#define OLED_I2C_ADDRESS 0x3C
#define OLED_SDA_GPIO 21
#define OLED_SCL_GPIO 22
#define OLED_I2C_CLOCK_HZ (100 * 1000)
#define OLED_I2C_TIMEOUT_MS 1000
#define OLED_RECOVERY_SETTLE_MS 40U
#define OLED_ROW_COUNT 8U
#define OLED_COL_COUNT 16U
#define OLED_PROGRESS_SLOTS 5U

static const char *TAG = "oled";

static u8x8_t s_u8x8;
static u8g2_esp32_i2c_ctx_t s_i2c_ctx;
static bool s_initialized;
static bool s_last_state_valid;
static char s_last_lines[OLED_ROW_COUNT][OLED_COL_COUNT + 1];

static void format_row(char dest[OLED_COL_COUNT + 1], const char *src)
{
    snprintf(dest, OLED_COL_COUNT + 1, "%-16.16s", src);
}

static bool row_is_blank(const char row[OLED_COL_COUNT + 1])
{
    for (size_t i = 0; i < OLED_COL_COUNT; i++) {
        if (row[i] != ' ') {
            return false;
        }
    }
    return true;
}

static void clear_cached_rows(void)
{
    memset(s_last_lines, 0, sizeof(s_last_lines));
}

static esp_err_t oled_hw_init(void)
{
    s_i2c_ctx = (u8g2_esp32_i2c_ctx_t){
        .cfg = {
            .i2c_port = OLED_I2C_PORT,
            .sda_pin = OLED_SDA_GPIO,
            .scl_pin = OLED_SCL_GPIO,
            .clk_hz = OLED_I2C_CLOCK_HZ,
            .dev_addr_7bit = OLED_I2C_ADDRESS,
            .timeout_ms = OLED_I2C_TIMEOUT_MS,
            .reset_pin = U8G2_ESP32_PIN_UNUSED,
        },
    };

    esp_err_t err = u8g2_esp32_i2c_set_default_context(&s_i2c_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set u8x8 i2c context failed: %s", esp_err_to_name(err));
        return err;
    }

    u8x8_Setup(&s_u8x8,
               u8x8_d_ssd1306_128x64_noname,
               u8x8_cad_ssd13xx_fast_i2c,
               u8x8_byte_esp32_hw_i2c,
               u8x8_gpio_and_delay_esp32_i2c);
    u8x8_SetI2CAddress(&s_u8x8, (uint8_t)(OLED_I2C_ADDRESS << 1));
    u8x8_InitDisplay(&s_u8x8);
    u8x8_SetPowerSave(&s_u8x8, 0);
    u8x8_SetFlipMode(&s_u8x8, 0);
    u8x8_SetFont(&s_u8x8, u8x8_font_victoriamedium8_r);
    u8x8_ClearDisplay(&s_u8x8);

    s_initialized = true;
    s_last_state_valid = false;
    clear_cached_rows();
    return ESP_OK;
}

static void draw_row(uint8_t row, const char text[OLED_COL_COUNT + 1])
{
    u8x8_ClearLine(&s_u8x8, row);
    if (!row_is_blank(text)) {
        u8x8_DrawString(&s_u8x8, 0, row, text);
    }
}

static void build_rows(char rows[OLED_ROW_COUNT][OLED_COL_COUNT + 1],
                       const float temperatures_c[2],
                       const uint64_t sensor_addresses[2],
                       bool wifi_connected,
                       bool mqtt_connected,
                       const char *ip_address,
                       uint32_t seconds_since_update,
                       uint8_t update_progress_percent)
{
    char temp_line[OLED_COL_COUNT + 1];
    char progress_line[OLED_COL_COUNT + 1];
    char progress_bar[OLED_COL_COUNT + 1];
    char sensor0_id[5];
    char sensor1_id[5];
    char bar_chars[OLED_PROGRESS_SLOTS + 1];

    for (size_t row = 0; row < OLED_ROW_COUNT; row++) {
        format_row(rows[row], "");
    }

    snprintf(sensor0_id, sizeof(sensor0_id), "%04llX",
             (unsigned long long)(sensor_addresses[0] & 0xFFFFULL));
    snprintf(sensor1_id, sizeof(sensor1_id), "%04llX",
             (unsigned long long)(sensor_addresses[1] & 0xFFFFULL));

    snprintf(temp_line, sizeof(temp_line), "%s %5.1fC", sensor0_id, temperatures_c[0]);
    format_row(rows[0], temp_line);

    snprintf(temp_line, sizeof(temp_line), "%s %5.1fC", sensor1_id, temperatures_c[1]);
    format_row(rows[1], temp_line);

    format_row(rows[2], ip_address);
    format_row(rows[3], wifi_connected ? "CONNECTED" : "CONNECTING");
    format_row(rows[4], mqtt_connected ? "MQTT OK" : "MQTT WAIT");

    snprintf(progress_line, sizeof(progress_line), "LOOP %lus %3u%%",
             (unsigned long)seconds_since_update,
             (unsigned)update_progress_percent);
    format_row(rows[5], progress_line);

    uint8_t filled = (seconds_since_update >= OLED_PROGRESS_SLOTS)
                         ? OLED_PROGRESS_SLOTS
                         : (uint8_t)seconds_since_update;
    for (uint8_t i = 0; i < OLED_PROGRESS_SLOTS; i++) {
        bar_chars[i] = (i < filled) ? '#' : '.';
    }
    bar_chars[OLED_PROGRESS_SLOTS] = '\0';
    snprintf(progress_bar, sizeof(progress_bar), "[%s]", bar_chars);
    format_row(rows[6], progress_bar);
    format_row(rows[7], "");
}

esp_err_t oled_init(void)
{
    esp_err_t err = oled_hw_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "u8x8 init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "u8x8 SSD1306 initialized on SDA GPIO %d, SCL GPIO %d",
             OLED_SDA_GPIO, OLED_SCL_GPIO);
    return ESP_OK;
}

esp_err_t oled_recover(void)
{
    if (s_i2c_ctx.bus_handle != NULL) {
        esp_err_t reset_err = i2c_master_bus_reset((i2c_master_bus_handle_t)s_i2c_ctx.bus_handle);
        if (reset_err != ESP_OK) {
            return reset_err;
        }
        vTaskDelay(pdMS_TO_TICKS(OLED_RECOVERY_SETTLE_MS));
    }

    esp_err_t err = oled_hw_init();
    return err;
}

esp_err_t oled_update(const float temperatures_c[2], bool wifi_connected,
                      bool mqtt_connected,
                      const uint64_t sensor_addresses[2],
                      const char *ip_address,
                      uint32_t seconds_since_update,
                      uint8_t update_progress_percent)
{
    if (!s_initialized || temperatures_c == NULL || sensor_addresses == NULL ||
        ip_address == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char rows[OLED_ROW_COUNT][OLED_COL_COUNT + 1];
    build_rows(rows, temperatures_c, sensor_addresses, wifi_connected, mqtt_connected,
               ip_address,
               seconds_since_update, update_progress_percent);

    for (uint8_t row = 0; row < OLED_ROW_COUNT; row++) {
        if (!s_last_state_valid || strcmp(rows[row], s_last_lines[row]) != 0) {
            draw_row(row, rows[row]);
            memcpy(s_last_lines[row], rows[row], OLED_COL_COUNT + 1);
        }
    }
    s_last_state_valid = true;
    return ESP_OK;
}
