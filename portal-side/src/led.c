/* led.c — minimal WS2812 driver over RMT (no led_strip dependency). */
#include "led.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"

#ifndef LED_GPIO
#define LED_GPIO 48          /* N16R8 onboard WS2812 (change if your board differs) */
#endif

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;

void led_init(void) {
    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = 10 * 1000 * 1000,        /* 0.1 µs per tick */
        .trans_queue_depth = 4,
    };
    if (rmt_new_tx_channel(&tx_cfg, &s_chan) != ESP_OK) { s_chan = NULL; return; }

    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },  /* WS2812 "0" */
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },  /* WS2812 "1" */
        .flags = { .msb_first = 1 },
    };
    if (rmt_new_bytes_encoder(&enc_cfg, &s_enc) != ESP_OK) { s_chan = NULL; return; }
    rmt_enable(s_chan);
}

void led_set(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_chan) return;
    uint8_t grb[3] = { g, r, b };                 /* WS2812 byte order is GRB */
    rmt_transmit_config_t tx = { .loop_count = 0 };
    rmt_transmit(s_chan, s_enc, grb, sizeof(grb), &tx);
    rmt_tx_wait_all_done(s_chan, 50);
}
