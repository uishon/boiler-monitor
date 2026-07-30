#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_log.h"

#include "oled.h"

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_I2C_PORT I2C_NUM_0
#define OLED_I2C_ADDRESS 0x3C
#define OLED_SDA_GPIO 21
#define OLED_SCL_GPIO 22
#define OLED_I2C_CLOCK_HZ (50 * 1000)
#define OLED_I2C_TIMEOUT_MS 50
#define OLED_SCL_WAIT_US 13000
#define OLED_DOT_COUNT 5
#define OLED_DOT_SIZE 3
#define OLED_DOT_GAP 2
#define OLED_DOT_ORIGIN_X 0
#define OLED_DOT_ORIGIN_Y 58

static const char *TAG = "oled";
static esp_lcd_panel_handle_t s_panel;
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_i2c_dev;
static uint8_t s_framebuffer[OLED_WIDTH * OLED_HEIGHT / 8];
static float s_last_temperatures_c[2];
static uint64_t s_last_sensor_addresses[2];
static bool s_last_wifi_connected;
static char s_last_ip_address[16];
static bool s_last_state_valid;

typedef struct {
    char character;
    uint8_t columns[5];
} glyph_t;

static const glyph_t s_font[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}},
    {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
    {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}},
    {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}},
    {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
    {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}},
    {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}},
    {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
    {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}},
    {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
    {'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
    {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}},
    {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}},
    {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
    {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}},
    {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}},
    {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
    {'W', {0x7F, 0x20, 0x18, 0x20, 0x7F}},
    {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
    {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
};

static const uint8_t *find_glyph(char character)
{
    for (size_t i = 0; i < sizeof(s_font) / sizeof(s_font[0]); i++) {
        if (s_font[i].character == character) {
            return s_font[i].columns;
        }
    }
    return s_font[0].columns;
}

static void set_pixel(uint8_t x, uint8_t y)
{
    if (x >= OLED_WIDTH || y >= OLED_HEIGHT) {
        return;
    }

    s_framebuffer[(y / 8) * OLED_WIDTH + x] |= (1U << (y % 8));
}

static void draw_text(uint8_t x, uint8_t y, const char *text)
{
    const uint8_t glyph_width = 5;
    const uint8_t glyph_height = 7;
    const uint8_t advance = 6;

    while (*text != '\0' && x <= OLED_WIDTH - glyph_width) {
        const uint8_t *columns = find_glyph(*text++);

        for (uint8_t column = 0; column < glyph_width; column++) {
            for (uint8_t row = 0; row < glyph_height; row++) {
                if (((columns[column] >> row) & 0x01U) == 0U) {
                    continue;
                }

                set_pixel(x + column, y + row);
            }
        }

        x += advance;
    }
}

static void clear_area(uint8_t x, uint8_t y, uint8_t width, uint8_t height)
{
    for (uint8_t yy = y; yy < (uint8_t)(y + height) && yy < OLED_HEIGHT; yy++) {
        for (uint8_t xx = x; xx < (uint8_t)(x + width) && xx < OLED_WIDTH; xx++) {
            s_framebuffer[(yy / 8) * OLED_WIDTH + xx] &= (uint8_t)~(1U << (yy % 8));
        }
    }
}

static void draw_filled_rect(uint8_t x, uint8_t y, uint8_t width, uint8_t height)
{
    for (uint8_t yy = y; yy < (uint8_t)(y + height) && yy < OLED_HEIGHT; yy++) {
        for (uint8_t xx = x; xx < (uint8_t)(x + width) && xx < OLED_WIDTH; xx++) {
            set_pixel(xx, yy);
        }
    }
}

static esp_err_t oled_tx_cmd(uint8_t cmd)
{
    uint8_t packet[2] = {0x00, cmd};
    return i2c_master_transmit(s_i2c_dev, packet, sizeof(packet), OLED_I2C_TIMEOUT_MS);
}

static esp_err_t oled_tx_page(uint8_t page, const uint8_t *data)
{
    esp_err_t err = oled_tx_cmd((uint8_t)(0xB0U | (page & 0x0FU)));
    if (err != ESP_OK) {
        return err;
    }

    err = oled_tx_cmd(0x00U);
    if (err != ESP_OK) {
        return err;
    }

    err = oled_tx_cmd(0x10U);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t packet[1 + OLED_WIDTH];
    packet[0] = 0x40U;
    memcpy(&packet[1], data, OLED_WIDTH);
    return i2c_master_transmit(s_i2c_dev, packet, sizeof(packet), OLED_I2C_TIMEOUT_MS);
}

static esp_err_t oled_tx_page_window(uint8_t page, uint8_t x_start, uint8_t x_end,
                                     const uint8_t *data)
{
    if (x_start > x_end || x_end >= OLED_WIDTH) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = oled_tx_cmd((uint8_t)(0xB0U | (page & 0x0FU)));
    if (err != ESP_OK) {
        return err;
    }

    err = oled_tx_cmd((uint8_t)(x_start & 0x0FU));
    if (err != ESP_OK) {
        return err;
    }

    err = oled_tx_cmd((uint8_t)(0x10U | ((x_start >> 4) & 0x0FU)));
    if (err != ESP_OK) {
        return err;
    }

    uint8_t width = (uint8_t)(x_end - x_start + 1);
    uint8_t packet[1 + OLED_WIDTH];
    packet[0] = 0x40U;
    memcpy(&packet[1], data + x_start, width);
    return i2c_master_transmit(s_i2c_dev, packet, (size_t)(1 + width), OLED_I2C_TIMEOUT_MS);
}

static esp_err_t oled_apply_orientation(void)
{
    esp_err_t err = oled_tx_cmd(0xA1U);
    if (err != ESP_OK) {
        return err;
    }

    return oled_tx_cmd(0xC8U);
}

static esp_err_t flush_region(uint8_t y_start, uint8_t y_end)
{
    if (y_start >= y_end || y_end > OLED_HEIGHT || (y_start % 8) != 0 ||
        (y_end % 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_i2c_bus == NULL || s_i2c_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = i2c_master_probe(s_i2c_bus, OLED_I2C_ADDRESS, OLED_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        (void)i2c_master_bus_reset(s_i2c_bus);
        return err;
    }

    uint8_t start_page = y_start / 8;
    uint8_t end_page = y_end / 8;
    for (uint8_t page = start_page; page < end_page; page++) {
        err = oled_tx_page(page, &s_framebuffer[page * OLED_WIDTH]);
        if (err != ESP_OK) {
            (void)i2c_master_bus_reset(s_i2c_bus);
            return err;
        }
    }

    return ESP_OK;
}

static esp_err_t flush_bottom_dots_window(void)
{
    if (s_i2c_bus == NULL || s_i2c_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = i2c_master_probe(s_i2c_bus, OLED_I2C_ADDRESS, OLED_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        (void)i2c_master_bus_reset(s_i2c_bus);
        return err;
    }

    uint8_t x_end = (uint8_t)(OLED_DOT_ORIGIN_X + (OLED_DOT_COUNT * OLED_DOT_SIZE) +
                              ((OLED_DOT_COUNT - 1) * OLED_DOT_GAP) - 1);
    err = oled_tx_page_window(7, OLED_DOT_ORIGIN_X, x_end,
                              &s_framebuffer[7 * OLED_WIDTH]);
    if (err != ESP_OK) {
        (void)i2c_master_bus_reset(s_i2c_bus);
    }

    return err;
}

static void draw_static_status(const float temperatures_c[2],
                               const uint64_t sensor_addresses[2],
                               bool wifi_connected, const char *ip_address)
{
    char line[22];
    char sensor0_label[5];
    char sensor1_label[5];

    if (sensor_addresses[0] != 0U) {
        snprintf(sensor0_label, sizeof(sensor0_label), "%04llX",
                 (unsigned long long)(sensor_addresses[0] & 0xFFFFULL));
    } else {
        snprintf(sensor0_label, sizeof(sensor0_label), "----");
    }

    if (sensor_addresses[1] != 0U) {
        snprintf(sensor1_label, sizeof(sensor1_label), "%04llX",
                 (unsigned long long)(sensor_addresses[1] & 0xFFFFULL));
    } else {
        snprintf(sensor1_label, sizeof(sensor1_label), "----");
    }

    snprintf(line, sizeof(line), "%s: %.2fC", sensor0_label, temperatures_c[0]);
    draw_text(0, 0, line);

    snprintf(line, sizeof(line), "%s: %.2fC", sensor1_label, temperatures_c[1]);
    draw_text(0, 9, line);

    snprintf(line, sizeof(line), "IP: %s", ip_address);
    draw_text(0, 18, line);

    draw_text(0, 27, wifi_connected ? "WIFI: CONNECTED" : "WIFI: CONNECTING");
}

static void draw_dynamic_status(uint32_t seconds_since_update,
                                uint8_t update_progress_percent)
{
    (void)update_progress_percent;

    uint8_t filled_dots = (seconds_since_update >= OLED_DOT_COUNT)
                              ? OLED_DOT_COUNT
                              : (uint8_t)seconds_since_update;
    uint8_t total_width = (uint8_t)((OLED_DOT_COUNT * OLED_DOT_SIZE) +
                                    ((OLED_DOT_COUNT - 1) * OLED_DOT_GAP));

    clear_area(OLED_DOT_ORIGIN_X, 56, total_width, 8);

    for (uint8_t i = 0; i < filled_dots; i++) {
        uint8_t x = (uint8_t)(OLED_DOT_ORIGIN_X + i * (OLED_DOT_SIZE + OLED_DOT_GAP));
        draw_filled_rect(x, OLED_DOT_ORIGIN_Y, OLED_DOT_SIZE, OLED_DOT_SIZE);
    }
}

esp_err_t oled_init(void)
{
    esp_err_t err;
    const i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = OLED_I2C_PORT,
        .sda_io_num = OLED_SDA_GPIO,
        .scl_io_num = OLED_SCL_GPIO,
        .flags.enable_internal_pullup = true,
    };
    err = i2c_new_master_bus(&bus_config, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create I2C bus failed: %s", esp_err_to_name(err));
        return err;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_I2C_ADDRESS,
        .scl_speed_hz = OLED_I2C_CLOCK_HZ,
        .scl_wait_us = OLED_SCL_WAIT_US,
    };
    err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add OLED I2C device failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_io_handle_t io_handle;
    const esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = OLED_I2C_ADDRESS,
        .scl_speed_hz = OLED_I2C_CLOCK_HZ,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_bit_offset = 6,
    };
    err = esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_config, &io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create panel I2C interface failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_lcd_panel_dev_config_t panel_config = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
    };
    err = esp_lcd_new_panel_ssd1306(io_handle, &panel_config, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create SSD1306 panel failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_reset(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reset SSD1306 panel failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_init(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "initialize SSD1306 panel failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_mirror(s_panel, true, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set SSD1306 mirror failed: %s", esp_err_to_name(err));
        return err;
    }

    err = oled_apply_orientation();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "apply SSD1306 orientation failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "turn on SSD1306 panel failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SSD1306 initialized on SDA GPIO %d, SCL GPIO %d",
             OLED_SDA_GPIO, OLED_SCL_GPIO);
    return ESP_OK;
}

esp_err_t oled_recover(void)
{
    if (s_i2c_bus == NULL || s_i2c_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "Attempting OLED recovery");

    esp_err_t err = i2c_master_bus_reset(s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus reset failed: %s", esp_err_to_name(err));
        return err;
    }

    err = oled_tx_cmd(0xAEU);
    if (err != ESP_OK) {
        return err;
    }

    err = oled_tx_cmd(0x20U);
    if (err != ESP_OK) {
        return err;
    }

    err = oled_tx_cmd(0x02U);
    if (err != ESP_OK) {
        return err;
    }

    err = oled_apply_orientation();
    if (err != ESP_OK) {
        return err;
    }

    err = oled_tx_cmd(0xA4U);
    if (err != ESP_OK) {
        return err;
    }

    err = oled_tx_cmd(0xA6U);
    if (err != ESP_OK) {
        return err;
    }

    err = oled_tx_cmd(0xAFU);
    if (err != ESP_OK) {
        return err;
    }

    s_last_state_valid = false;
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    return flush_region(0, OLED_HEIGHT);
}

esp_err_t oled_update(const float temperatures_c[2], bool wifi_connected,
                      const uint64_t sensor_addresses[2],
                      const char *ip_address,
                      uint32_t seconds_since_update,
                      uint8_t update_progress_percent)
{
    if (s_panel == NULL || temperatures_c == NULL || sensor_addresses == NULL ||
        ip_address == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    bool static_changed = !s_last_state_valid ||
                          temperatures_c[0] != s_last_temperatures_c[0] ||
                          temperatures_c[1] != s_last_temperatures_c[1] ||
                          sensor_addresses[0] != s_last_sensor_addresses[0] ||
                          sensor_addresses[1] != s_last_sensor_addresses[1] ||
                          wifi_connected != s_last_wifi_connected ||
                          strncmp(ip_address, s_last_ip_address,
                                  sizeof(s_last_ip_address)) != 0;

    if (static_changed) {
        memset(s_framebuffer, 0, sizeof(s_framebuffer));
        draw_static_status(temperatures_c, sensor_addresses, wifi_connected,
                           ip_address);
        draw_dynamic_status(seconds_since_update, update_progress_percent);

        s_last_temperatures_c[0] = temperatures_c[0];
        s_last_temperatures_c[1] = temperatures_c[1];
        s_last_sensor_addresses[0] = sensor_addresses[0];
        s_last_sensor_addresses[1] = sensor_addresses[1];
        s_last_wifi_connected = wifi_connected;
        snprintf(s_last_ip_address, sizeof(s_last_ip_address), "%s", ip_address);
        s_last_state_valid = true;

        return flush_region(0, OLED_HEIGHT);
    }

    draw_dynamic_status(seconds_since_update, update_progress_percent);
    return flush_bottom_dots_window();
}
