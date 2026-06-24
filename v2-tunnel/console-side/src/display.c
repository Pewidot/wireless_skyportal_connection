/*
 * display.c — ST7735 driver for the LilyGo T-Dongle-S3 (160x80, SPI).
 *
 * Config taken verbatim from LilyGo's factory_screen example:
 * SPI2, MOSI=3 CLK=5 CS=4 DC=2 RST=1 BL=38 (backlight active-LOW), BGR, invert,
 * set_gap(1,26) + swap_xy + mirror(false,true) for the 160x80 landscape view.
 * Uses LilyGo's esp_lcd_st7735 panel driver (esp_lcd_st7735.c).
 */
#include "display.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7735.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "font8x8.h"

#define LCD_HOST   SPI2_HOST
#define PIN_MOSI   3
#define PIN_CLK    5
#define PIN_CS     4
#define PIN_DC     2
#define PIN_RST    1
#define PIN_BL     38         /* backlight: 0 = on, 1 = off (active low) */
#define LCD_W      160
#define LCD_H      80

static const char *TAG = "lcd";
static esp_lcd_panel_handle_t s_panel;

void display_init(void) {
    gpio_config_t bk = { .pin_bit_mask = 1ULL << PIN_BL, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&bk);
    gpio_set_level(PIN_BL, 1);                 /* off while initialising */

    spi_bus_config_t buscfg = ST7735_PANEL_BUS_SPI_CONFIG(PIN_CLK, PIN_MOSI,
                                                          LCD_W * LCD_H * sizeof(uint16_t));
    if (spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "spi bus init failed"); return;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t iocfg = ST7735_PANEL_IO_SPI_CONFIG(PIN_CS, PIN_DC, NULL, NULL);
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &iocfg, &io) != ESP_OK) {
        ESP_LOGE(TAG, "panel io init failed"); return;
    }

    esp_lcd_panel_dev_config_t panelcfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7735(io, &panelcfg, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "st7735 panel init failed"); s_panel = NULL; return;
    }

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_set_gap(s_panel, 1, 26);
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, false, true);
    esp_lcd_panel_disp_on_off(s_panel, true);

    gpio_set_level(PIN_BL, 0);                  /* backlight ON (active low) */
    display_fill(RGB565(0, 0, 0));
    ESP_LOGI(TAG, "ST7735 (T-Dongle-S3) init done");
}

void display_fill(uint16_t color) {
    if (!s_panel) return;
    static uint16_t strip[LCD_W * 20];
    for (int i = 0; i < LCD_W * 20; i++) strip[i] = color;
    for (int y = 0; y < LCD_H; y += 20) {
        int h = (y + 20 <= LCD_H) ? 20 : (LCD_H - y);
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_W, y + h, strip);
    }
}

void display_char(int x, int y, char ch, uint16_t fg, uint16_t bg) {
    if (!s_panel) return;
    unsigned char c = (unsigned char)ch;
    if (c >= 128) c = '?';
    uint16_t buf[8 * 8];
    for (int r = 0; r < 8; r++) {
        unsigned char bits = (unsigned char)font8x8_basic[c][r];
        for (int b = 0; b < 8; b++)
            buf[r * 8 + b] = ((bits >> b) & 1) ? fg : bg;
    }
    esp_lcd_panel_draw_bitmap(s_panel, x, y, x + 8, y + 8, buf);
}

void display_text(int x, int y, const char *s, uint16_t fg, uint16_t bg) {
    while (*s && x + 8 <= LCD_W) {
        display_char(x, y, *s++, fg, bg);
        x += 8;
    }
}
