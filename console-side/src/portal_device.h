/* portal_device.h — Portal-of-Power emulation logic (M2). */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Start with an empty portal (figures arrive via the ESP-NOW relay). */
void portal_device_init_figure(void);

/* ---- fed by the ESP-NOW relay (link) ---- */
/* Mark slot present with toy_id (synthesises block 1 so it's identifiable
 * before the full image arrives). */
void portal_device_set_present(uint8_t slot, uint16_t toy_id);
/* Store the full 1 KB figure image for a slot. */
void portal_device_set_image(uint8_t slot, const uint8_t *img1024);
/* Remove a figure. */
void portal_device_clear_slot(uint8_t slot);
/* Clear any slot not set in `mask` (keeps emulation in sync with the portal). */
void portal_device_sync_mask(uint16_t mask);

/* Start the response/status streaming task. Call after tinyusb_driver_install. */
void portal_device_start(void);

/* True once the host has sent Activate (i.e. it is actively talking to us). */
bool portal_device_activated(void);

/* Toy-ID of the figure currently on the (emulated) portal, or 0 if none. */
uint16_t portal_device_current_toy(void);
