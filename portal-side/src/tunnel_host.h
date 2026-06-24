/* tunnel_host.h — USB host that forwards the real portal's HID reports verbatim. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Called for every interrupt-IN report from the portal (Wi-Fi/USB task ctx). */
typedef void (*tunnel_in_cb)(const uint8_t *report, int len);

void      tunnel_host_init(tunnel_in_cb on_in);
bool      tunnel_host_connected(void);
/* Send a raw command / OUT report to the portal (via SET_REPORT / control). */
esp_err_t tunnel_host_send(const uint8_t *buf, int len);
/* Send audio / trap-LED data to the portal's interrupt OUT endpoint (0x02). */
esp_err_t tunnel_host_send_out(const uint8_t *buf, int len);
