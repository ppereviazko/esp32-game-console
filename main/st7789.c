#include "st7789.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lcd";

#define PIN_SCLK GPIO_NUM_18
#define PIN_MOSI GPIO_NUM_23
#define PIN_DC GPIO_NUM_21
#define PIN_RST GPIO_NUM_22
#define PIN_CS GPIO_NUM_5
#define ROW_OFFSET 0

#define CMD_SWRESET 0x01
#define CMD_SLPOUT 0x11
#define CMD_NORON 0x13
#define CMD_INVON 0x21
#define CMD_DISPON 0x29
#define CMD_CASET 0x2A
#define CMD_RASET 0x2B
#define CMD_RAMWR 0x2C
#define CMD_COLMOD 0x3A
#define CMD_MADCTL 0x36

static spi_device_handle_t s_spi;

static void lcd_cmd(uint8_t cmd)
{
    gpio_set_level(PIN_DC, 0);
    spi_transaction_t t = {.length = 8, .tx_buffer = &cmd};
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void lcd_data(const void *data, size_t len)
{
    if (len == 0) {
        return;
    }
    gpio_set_level(PIN_DC, 1);
    spi_transaction_t t = {.length = len * 8, .tx_buffer = data};
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void lcd_bytes(uint8_t cmd, const uint8_t *data, size_t len)
{
    lcd_cmd(cmd);
    lcd_data(data, len);
}

void st7789_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_DC) | (1ULL << PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(PIN_DC, 0);
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * 20 * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 10 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_CS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &dev, &s_spi));

    lcd_cmd(CMD_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_cmd(CMD_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_bytes(CMD_MADCTL, (uint8_t[]){0x00}, 1);
    lcd_bytes(CMD_COLMOD, (uint8_t[]){0x55}, 1);
    lcd_bytes(0xB2, (uint8_t[]){0x0C, 0x0C, 0x00, 0x33, 0x33}, 5);
    lcd_bytes(0xB7, (uint8_t[]){0x35}, 1);
    lcd_bytes(0xBB, (uint8_t[]){0x19}, 1);
    lcd_bytes(0xC0, (uint8_t[]){0x2C}, 1);
    lcd_bytes(0xC2, (uint8_t[]){0x01}, 1);
    lcd_bytes(0xC3, (uint8_t[]){0x12}, 1);
    lcd_bytes(0xC4, (uint8_t[]){0x20}, 1);
    lcd_bytes(0xC6, (uint8_t[]){0x0F}, 1);
    lcd_bytes(0xD0, (uint8_t[]){0xA4, 0xA1}, 2);
    lcd_bytes(0xE0, (uint8_t[]){0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23}, 14);
    lcd_bytes(0xE1, (uint8_t[]){0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23}, 14);
    lcd_cmd(CMD_INVON);
    lcd_cmd(CMD_NORON);
    lcd_cmd(CMD_DISPON);
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "st7789 ready");
}

void st7789_fill_rect(int y, int h, uint16_t color)
{
    uint16_t y0 = (uint16_t)(y + ROW_OFFSET);
    uint16_t y1 = (uint16_t)(y0 + h - 1);
    uint8_t caset[4] = {0, 0, (uint8_t)((LCD_WIDTH - 1) >> 8), (uint8_t)((LCD_WIDTH - 1) & 0xFF)};
    uint8_t raset[4] = {y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF};
    lcd_cmd(CMD_CASET);
    lcd_data(caset, sizeof(caset));
    lcd_cmd(CMD_RASET);
    lcd_data(raset, sizeof(raset));
    lcd_cmd(CMD_RAMWR);

    uint8_t hi = color >> 8;
    uint8_t lo = color & 0xFF;
    uint8_t line[LCD_WIDTH * 2];
    for (int x = 0; x < LCD_WIDTH; x++) {
        line[x * 2] = hi;
        line[x * 2 + 1] = lo;
    }
    gpio_set_level(PIN_DC, 1);
    for (int row = 0; row < h; row++) {
        spi_transaction_t t = {.length = sizeof(line) * 8, .tx_buffer = line};
        ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
    }
}

void st7789_fill_box(int x, int y, int w, int h, uint16_t color)
{
    uint16_t x1 = (uint16_t)(x + w - 1);
    uint16_t y0 = (uint16_t)(y + ROW_OFFSET);
    uint16_t y1 = (uint16_t)(y0 + h - 1);
    uint8_t caset[4] = {(uint8_t)(x >> 8), (uint8_t)(x & 0xFF), (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF)};
    uint8_t raset[4] = {y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF};
    lcd_cmd(CMD_CASET);
    lcd_data(caset, sizeof(caset));
    lcd_cmd(CMD_RASET);
    lcd_data(raset, sizeof(raset));
    lcd_cmd(CMD_RAMWR);

    uint8_t hi = (uint8_t)(color >> 8);
    uint8_t lo = (uint8_t)(color & 0xFF);
    uint8_t line[LCD_WIDTH * 2];
    for (int i = 0; i < w; i++) {
        line[i * 2] = hi;
        line[i * 2 + 1] = lo;
    }
    gpio_set_level(PIN_DC, 1);
    for (int row = 0; row < h; row++) {
        spi_transaction_t t = {.length = (size_t)w * 2 * 8, .tx_buffer = line};
        ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
    }
}

void st7789_draw_rgb565(int x, int y, int w, int h, const uint8_t *pixels)
{
    uint16_t x1 = (uint16_t)(x + w - 1);
    uint16_t y0 = (uint16_t)(y + ROW_OFFSET);
    uint16_t y1 = (uint16_t)(y0 + h - 1);
    uint8_t caset[4] = {(uint8_t)(x >> 8), (uint8_t)(x & 0xFF), (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF)};
    uint8_t raset[4] = {y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF};
    lcd_cmd(CMD_CASET);
    lcd_data(caset, sizeof(caset));
    lcd_cmd(CMD_RASET);
    lcd_data(raset, sizeof(raset));
    lcd_cmd(CMD_RAMWR);

    uint8_t line[LCD_WIDTH * 2];
    gpio_set_level(PIN_DC, 1);
    for (int row = 0; row < h; row++) {
        memcpy(line, pixels + (size_t)row * w * 2, (size_t)w * 2);
        spi_transaction_t t = {.length = (size_t)w * 2 * 8, .tx_buffer = line};
        ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
    }
}
