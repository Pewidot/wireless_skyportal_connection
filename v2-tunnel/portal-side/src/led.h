/* led.h — single onboard WS2812 (N16R8, GPIO48) via RMT. */
#pragma once
#include <stdint.h>
void led_init(void);
void led_set(uint8_t r, uint8_t g, uint8_t b);
