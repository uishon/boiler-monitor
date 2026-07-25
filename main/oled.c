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
#define OLED_I2C_CLOCK_HZ (100 * 1000)

static const char *TAG = "oled";
static esp_lcd_panel_handle_t s_panel;
static uint8_t s_framebuffer[OLED_WIDTH * OLED_HEIGHT / 8];

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

esp_err_t oled_init(void)
{
    esp_err_t err;
    i2c_master_bus_handle_t i2c_bus;
    const i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = OLED_I2C_PORT,
        .sda_io_num = OLED_SDA_GPIO,
        .scl_io_num = OLED_SCL_GPIO,
        .flags.enable_internal_pullup = true,
    };
    err = i2c_new_master_bus(&bus_config, &i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create I2C bus failed: %s", esp_err_to_name(err));
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
    err = esp_lcd_new_panel_io_i2c(i2c_bus, &io_config, &io_handle);
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

    err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "turn on SSD1306 panel failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SSD1306 initialized on SDA GPIO %d, SCL GPIO %d",
             OLED_SDA_GPIO, OLED_SCL_GPIO);
    return ESP_OK;
}

esp_err_t oled_update(const float temperatures_c[2], bool wifi_connected,
                      const char *ip_address)
{
    if (s_panel == NULL || temperatures_c == NULL || ip_address == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char line[22];
    memset(s_framebuffer, 0, sizeof(s_framebuffer));

    snprintf(line, sizeof(line), "S1: %.2fC", temperatures_c[0]);
    draw_text(0, 0, line);

    snprintf(line, sizeof(line), "S2: %.2fC", temperatures_c[1]);
    draw_text(0, 9, line);

    snprintf(line, sizeof(line), "IP: %s", ip_address);
    draw_text(0, 18, line);

    draw_text(0, 27, wifi_connected ? "WIFI: CONNECTED" : "WIFI: CONNECTING");

    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, OLED_WIDTH, OLED_HEIGHT,
                                     s_framebuffer);
}
