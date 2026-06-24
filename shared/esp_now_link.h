/*
 * esp_now_link.h — auto-discovering ESP-NOW link shared by both boards.
 *
 * Both sides broadcast HELLO until they hear the other role, then pair (store
 * the peer MAC) and exchange unicast messages (link_protocol.h). The link
 * self-heals: if the peer goes quiet, it drops back to discovery.
 *
 * Compiled into both firmwares (copied into each src/).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Called for every valid incoming message (in the Wi-Fi task context — keep it
 * light). `type` is a LINK_T_* value; `data`/`len` is the whole message. */
typedef void (*link_recv_handler_t)(uint8_t type, const uint8_t *data, int len, int rssi);

/* role = LINK_ROLE_PORTAL or LINK_ROLE_CONSOLE. */
void link_init(uint8_t role, link_recv_handler_t handler);

bool link_is_linked(void);
int  link_rssi(void);                       /* last RSSI from the peer (dBm) */

/* Send a message to the paired peer. Returns ESP_FAIL if not linked. */
esp_err_t link_send(const void *data, size_t len);
