/*
 * status_led.h — onboard WS2812 status indicator for the portal-side board.
 *
 *   white  = booting
 *   orange = no portal detected (waiting / USB not enumerating)
 *   blue   = portal connected & ready, no figure
 *   green  = a figure is on the portal
 *   red    = error
 *
 * LED GPIO is set via -DSTATUS_LED_GPIO (platformio.ini), default 48.
 */
#pragma once

typedef enum {
    LED_BOOT,
    LED_WAITING,        // no portal (or USB not enumerated)
    LED_PORTAL_READY,   // portal up, no figure
    LED_FIGURE,         // figure(s) on the portal
    LED_ERROR,
} led_state_t;

void status_led_init(void);
void status_led_set(led_state_t state);
