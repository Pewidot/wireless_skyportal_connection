/*
 * portal_device.c — emulate the Portal of Power, serving figures that arrive
 * over the ESP-NOW relay (per slot). The host (console / PC tool) sends commands
 * as HID SET_REPORTs; we answer Status (present mask) and Query (block data)
 * from the replicated per-slot state.
 */
#include <string.h>
#include "tusb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "portal_device.h"
#include "sky_protocol.h"

static const char *TAG = "portal-dev";

static uint8_t       s_img[SKY_MAX_SLOTS][SKY_DUMP_SIZE];  /* per-slot 1 KB image */
static uint16_t      s_toy[SKY_MAX_SLOTS];
static volatile uint16_t s_present;        /* bit i = slot i occupied */
static volatile bool s_activated;
static uint8_t       s_status_counter;
static QueueHandle_t s_resp_q;

void portal_device_init_figure(void) {
    memset(s_img, 0, sizeof(s_img));
    memset(s_toy, 0, sizeof(s_toy));
    s_present = 0;
}

/* ---- relay-fed state ---- */
void portal_device_set_present(uint8_t slot, uint16_t toy_id) {
    if (slot >= SKY_MAX_SLOTS) return;
    s_toy[slot] = toy_id;
    uint8_t *b1 = &s_img[slot][SKY_BLOCK_TOY_ID * SKY_BLOCK_SIZE];
    b1[0] = toy_id & 0xFF;
    b1[1] = (toy_id >> 8) & 0xFF;
    s_present |= (uint16_t)(1u << slot);
}
void portal_device_set_image(uint8_t slot, const uint8_t *img1024) {
    if (slot >= SKY_MAX_SLOTS) return;
    memcpy(s_img[slot], img1024, SKY_DUMP_SIZE);
    s_present |= (uint16_t)(1u << slot);
}
void portal_device_clear_slot(uint8_t slot) {
    if (slot >= SKY_MAX_SLOTS) return;
    s_present &= (uint16_t)~(1u << slot);
    s_toy[slot] = 0;
}
void portal_device_sync_mask(uint16_t mask) {
    for (int i = 0; i < SKY_MAX_SLOTS; i++)
        if (!(mask & (1u << i)) && (s_present & (1u << i))) portal_device_clear_slot(i);
}

bool portal_device_activated(void) { return s_activated; }

uint16_t portal_device_current_toy(void) {
    for (int i = 0; i < SKY_MAX_SLOTS; i++)
        if (s_present & (1u << i)) return s_toy[i];
    return 0;
}

/* ---- command handling ---- */
static void push_resp(const uint8_t *data, int len) {
    uint8_t r[SKY_REPORT_LEN];
    memset(r, 0, sizeof(r));
    memcpy(r, data, len < SKY_REPORT_LEN ? len : SKY_REPORT_LEN);
    if (s_resp_q) xQueueSend(s_resp_q, r, 0);
}

static void handle_command(const uint8_t *b, int n) {
    switch (b[0]) {
    case SKY_CMD_READY: {
        /* Report as a Traptanium (Trap Team) portal for maximum game
         * compatibility — Trap Team only accepts this portal type, and it is
         * backward-compatible with the older games. */
        uint8_t r[3] = { SKY_CMD_READY, 0x02, 0x18 };
        push_resp(r, 3);
        break;
    }
    case SKY_CMD_ACTIVATE: {
        s_activated = (n > 1) && b[1];
        uint8_t r[4] = { SKY_CMD_ACTIVATE, (uint8_t)(s_activated ? 1 : 0), 0xFF, 0x77 };
        push_resp(r, 4);
        break;
    }
    case SKY_CMD_COLOR: {
        uint8_t r[4] = { SKY_CMD_COLOR, b[1], b[2], b[3] };
        push_resp(r, 4);
        break;
    }
    case SKY_CMD_QUERY: {
        uint8_t slot = b[1] & 0x0F, block = b[2];
        uint8_t r[SKY_REPORT_LEN];
        memset(r, 0, sizeof(r));
        r[0] = SKY_CMD_QUERY;
        r[2] = block;
        if ((s_present & (1u << slot)) && block < SKY_NUM_BLOCKS) {
            r[1] = (uint8_t)(0x10 + slot);
            memcpy(&r[3], &s_img[slot][block * SKY_BLOCK_SIZE], SKY_BLOCK_SIZE);
        } else {
            r[1] = 0x01;
        }
        push_resp(r, SKY_REPORT_LEN);
        break;
    }
    case SKY_CMD_WRITE: {
        uint8_t slot = b[1] & 0x0F, block = b[2];
        if (slot < SKY_MAX_SLOTS && block < SKY_NUM_BLOCKS)
            memcpy(&s_img[slot][block * SKY_BLOCK_SIZE], &b[3], SKY_BLOCK_SIZE);
        uint8_t r[3] = { SKY_CMD_WRITE, 0x00, block };
        push_resp(r, 3);
        break;
    }
    default: break;
    }
}

static void build_status(uint8_t *r) {
    memset(r, 0, SKY_REPORT_LEN);
    r[0] = SKY_CMD_STATUS;
    uint32_t slots = 0;
    for (int i = 0; i < SKY_MAX_SLOTS; i++)
        if (s_present & (1u << i)) slots |= (1u << (2 * i));   /* present bit */
    r[1] = slots & 0xFF;
    r[2] = (slots >> 8) & 0xFF;
    r[3] = (slots >> 16) & 0xFF;
    r[4] = (slots >> 24) & 0xFF;
    r[5] = s_status_counter++;
    r[6] = 0x01;
}

static void tx_task(void *arg) {
    uint8_t r[SKY_REPORT_LEN];
    TickType_t last_status = 0;
    for (;;) {
        if (!tud_mounted() || !tud_hid_ready()) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
        if (xQueueReceive(s_resp_q, r, 0) == pdTRUE) {
            tud_hid_report(0, r, SKY_REPORT_LEN);
        } else if (s_activated && (xTaskGetTickCount() - last_status) >= pdMS_TO_TICKS(20)) {
            build_status(r);
            tud_hid_report(0, r, SKY_REPORT_LEN);
            last_status = xTaskGetTickCount();
        } else {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
}

void portal_device_start(void) {
    s_resp_q = xQueueCreate(16, SKY_REPORT_LEN);
    xTaskCreate(tx_task, "portal_tx", 4096, NULL, 5, NULL);
}

/* ---- TinyUSB HID callbacks ---- */
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void) instance; (void) report_id; (void) report_type; (void) buffer; (void) reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void) instance; (void) report_id; (void) report_type;
    if (bufsize >= 1) handle_command(buffer, bufsize);
}
