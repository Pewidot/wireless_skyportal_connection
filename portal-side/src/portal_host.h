/*
 * portal_host.h — M1 USB-host driver for a real Skylanders Portal of Power.
 *
 * Installs the ESP-IDF USB host + HID host driver, talks the portal protocol
 * (sky_protocol.h), and reports figures via callbacks. A single "portal task"
 * owns all portal I/O so command/response ordering stays correct (mirrors the
 * Python reader we verified).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "sky_protocol.h"

/* Fired when a figure is identified on `slot` (toy id read from block 1). */
typedef void (*portal_present_cb)(int slot, uint16_t toy_id);
/* Fired when the figure on `slot` is removed. */
typedef void (*portal_remove_cb)(int slot);

void portal_host_set_callbacks(portal_present_cb on_present, portal_remove_cb on_remove);

/* Install USB host + HID host and start the portal task. */
esp_err_t portal_host_init(void);

/* True once a portal is connected and activated. */
bool portal_host_connected(void);

/* Read all 64 blocks (1024 bytes) of the figure on `slot` into `out`.
 * Returns the number of blocks that could NOT be read (0 == perfect dump).
 * Runs synchronously inside the portal task; safe to call from any task. */
int portal_host_dump(int slot, uint8_t out[SKY_DUMP_SIZE]);
