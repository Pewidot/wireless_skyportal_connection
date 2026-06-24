/* status_led.c — drive the single onboard WS2812 via the led_strip component. */
#include "status_led.h"
#include "led_strip.h"
#include "esp_log.h"

#ifndef STATUS_LED_GPIO
#define STATUS_LED_GPIO 48
#endif

static const char *TAG = "led";
static led_strip_handle_t s_strip;

void status_led_init(void) {
    led_strip_config_t strip_config = {
        .strip_gpio_num = STATUS_LED_GPIO,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags = { .with_dma = false },
    };
    if (led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip) != ESP_OK) {
        ESP_LOGW(TAG, "WS2812 init failed (pin %d?)", STATUS_LED_GPIO);
        s_strip = NULL;
        return;
    }
    status_led_set(LED_BOOT);
}

void status_led_set(led_state_t state) {
    if (!s_strip) return;
    uint8_t r = 0, g = 0, b = 0;
    switch (state) {
    case LED_BOOT:         r = 18; g = 18; b = 18; break;  // dim white
    case LED_WAITING:      r = 45; g = 18; b = 0;  break;  // orange
    case LED_PORTAL_READY: r = 0;  g = 0;  b = 60; break;  // blue
    case LED_FIGURE:       r = 0;  g = 70; b = 0;  break;  // green
    case LED_ERROR:        r = 80; g = 0;  b = 0;  break;  // red
    }
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}
