/*
 * portal_host.c — M1: drive a real Portal of Power over USB host.
 *
 * Built on the ESP-IDF USB Host Library + the `espressif/usb_host_hid` managed
 * component. Commands go out as SET_REPORT (Output) control transfers (the only
 * write path the portal honours); Status/Query responses arrive as HID input
 * reports on the interrupt-IN endpoint and are funnelled through one queue to a
 * single "portal task", so command/response ordering is deterministic.
 *
 * NOTE: first cut for on-hardware bring-up. Likely tuning points are marked TODO
 * (endpoint MPS, VID/PID filtering, timing). Console/logs are on UART0.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"

#include "portal_host.h"
#include "sky_protocol.h"

#define HID_REPORT_OUTPUT 2          /* HID report type "Output" for SET_REPORT */
#define REPORT_QUEUE_LEN  32

static const char *TAG = "portal";

static QueueHandle_t          s_report_queue;
static SemaphoreHandle_t      s_dump_done;
static volatile hid_host_device_handle_t s_hid_dev;
static volatile bool          s_connected;
static volatile bool          s_just_connected;

static portal_present_cb      s_on_present;
static portal_remove_cb       s_on_remove;

/* Dump request, serviced inside the portal task. */
static volatile bool          s_dump_req;
static int                    s_dump_slot;
static uint8_t               *s_dump_out;
static volatile int           s_dump_missing;

/* ------------------------------------------------------------------ */
void portal_host_set_callbacks(portal_present_cb on_present, portal_remove_cb on_remove) {
    s_on_present = on_present;
    s_on_remove  = on_remove;
}

bool portal_host_connected(void) { return s_connected; }

static void send_cmd(const uint8_t buf[SKY_REPORT_LEN]) {
    if (!s_hid_dev) return;
    esp_err_t e = hid_class_request_set_report((hid_host_device_handle_t)s_hid_dev,
                                               HID_REPORT_OUTPUT, 0,
                                               (uint8_t *)buf, SKY_REPORT_LEN);
    if (e != ESP_OK) ESP_LOGD(TAG, "set_report err 0x%x", e);
}

/* ------------------------------------------------------------------ */
/* USB host / HID host plumbing                                        */
/* ------------------------------------------------------------------ */
static void iface_cb(hid_host_device_handle_t dev,
                     const hid_host_interface_event_t event, void *arg) {
    uint8_t data[64];
    size_t len = 0;
    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        if (hid_host_device_get_raw_input_report_data(dev, data, sizeof(data), &len) == ESP_OK
                && len > 0) {
            uint8_t rep[SKY_REPORT_LEN];
            memset(rep, 0, sizeof(rep));
            memcpy(rep, data, len < SKY_REPORT_LEN ? len : SKY_REPORT_LEN);
            xQueueSend(s_report_queue, rep, 0);   /* drop if full (status flood) */
        }
        break;
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "portal disconnected");
        s_connected = false;
        s_hid_dev = NULL;
        hid_host_device_close(dev);
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "transfer error");
        break;
    default:
        break;
    }
}

static void device_cb(hid_host_device_handle_t dev,
                      const hid_host_driver_event_t event, void *arg) {
    if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
        /* TODO: filter by VID/PID (0x1430/0x0150). For this rig the portal is
         * the only HID device on the host, so we accept the first one. */
        const hid_host_device_config_t cfg = { .callback = iface_cb, .callback_arg = NULL };
        if (hid_host_device_open(dev, &cfg) != ESP_OK) { ESP_LOGE(TAG, "open failed"); return; }
        if (hid_host_device_start(dev) != ESP_OK)      { ESP_LOGE(TAG, "start failed"); return; }
        s_hid_dev = dev;
        s_connected = true;
        s_just_connected = true;
        ESP_LOGI(TAG, "portal connected");
    }
}

static void usb_lib_task(void *arg) {
    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

/* ------------------------------------------------------------------ */
/* Portal session logic (single owner of all portal I/O)               */
/* ------------------------------------------------------------------ */
/* Returns missing-block count, or -1 if the figure was lifted mid-dump. */
static int do_dump(int slot, uint8_t *out) {
    uint8_t cmd[SKY_REPORT_LEN], rep[SKY_REPORT_LEN];
    int missing = 0;
    for (int b = 0; b < SKY_NUM_BLOCKS; b++) {
        bool ok = false;
        for (int attempt = 0; attempt < 4 && !ok; attempt++) {
            sky_build_query(cmd, (uint8_t)slot, (uint8_t)b);
            send_cmd(cmd);
            TickType_t t0 = xTaskGetTickCount();
            while ((xTaskGetTickCount() - t0) < pdMS_TO_TICKS(80)) {
                if (xQueueReceive(s_report_queue, rep, pdMS_TO_TICKS(20)) == pdTRUE) {
                    if (rep[0] == SKY_CMD_STATUS) {
                        /* figure removed mid-dump → abort instead of retrying every
                         * block (which would hang the reader for ~a minute) */
                        if (!sky_slot_present(sky_status_slots(rep), slot)) {
                            ESP_LOGI(TAG, "dump slot %d aborted (figure removed)", slot);
                            return -1;
                        }
                    } else if (sky_is_query_resp(rep) && sky_query_ok(rep)
                               && sky_query_slot(rep) == slot && sky_query_block(rep) == b) {
                        memcpy(out + b * SKY_BLOCK_SIZE, sky_query_data(rep), SKY_BLOCK_SIZE);
                        ok = true;
                        break;
                    }
                }
            }
        }
        if (!ok) { memset(out + b * SKY_BLOCK_SIZE, 0, SKY_BLOCK_SIZE); missing++; }
    }
    ESP_LOGI(TAG, "dump slot %d: %d/%d blocks missing", slot, missing, SKY_NUM_BLOCKS);
    return missing;
}

static void portal_task(void *arg) {
    uint8_t cmd[SKY_REPORT_LEN], rep[SKY_REPORT_LEN];
    int64_t last_query[SKY_MAX_SLOTS] = {0};
    bool identified[SKY_MAX_SLOTS] = {false};

    for (;;) {
        if (s_just_connected) {
            s_just_connected = false;
            sky_build_ready(cmd);            send_cmd(cmd); vTaskDelay(pdMS_TO_TICKS(20));
            sky_build_activate(cmd, 1);      send_cmd(cmd); vTaskDelay(pdMS_TO_TICKS(20));
            sky_build_color(cmd, 0, 0x44, 0xFF); send_cmd(cmd);
            memset(identified, 0, sizeof(identified));
            memset(last_query, 0, sizeof(last_query));
            ESP_LOGI(TAG, "portal activated");
        }

        if (s_dump_req) {
            s_dump_req = false;
            s_dump_missing = s_connected ? do_dump(s_dump_slot, s_dump_out) : SKY_NUM_BLOCKS;
            xSemaphoreGive(s_dump_done);
            continue;
        }

        if (!s_connected) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

        if (xQueueReceive(s_report_queue, rep, pdMS_TO_TICKS(50)) != pdTRUE) continue;

        if (rep[0] == SKY_CMD_STATUS) {
            uint32_t slots = sky_status_slots(rep);
            int64_t now = esp_timer_get_time() / 1000;
            for (int i = 0; i < SKY_MAX_SLOTS; i++) {
                bool present = sky_slot_present(slots, i);
                if (present && !identified[i]) {
                    if (now - last_query[i] > 400) {            /* (re)identify */
                        last_query[i] = now;
                        sky_build_query(cmd, (uint8_t)i, SKY_BLOCK_TOY_ID);
                        send_cmd(cmd);
                    }
                } else if (!present && identified[i]) {
                    identified[i] = false;
                    if (s_on_remove) s_on_remove(i);
                }
            }
        } else if (sky_is_query_resp(rep) && sky_query_ok(rep)
                   && sky_query_block(rep) == SKY_BLOCK_TOY_ID) {
            int slot = sky_query_slot(rep);
            if (slot >= 0 && slot < SKY_MAX_SLOTS && !identified[slot]) {
                uint16_t toy = sky_query_data(rep)[0] | (sky_query_data(rep)[1] << 8);
                identified[slot] = true;
                if (s_on_present) s_on_present(slot, toy);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
int portal_host_dump(int slot, uint8_t out[SKY_DUMP_SIZE]) {
    if (!s_connected) return SKY_NUM_BLOCKS;
    s_dump_slot = slot;
    s_dump_out = out;
    s_dump_missing = SKY_NUM_BLOCKS;
    xSemaphoreTake(s_dump_done, 0);          /* clear stale signal */
    s_dump_req = true;
    if (xSemaphoreTake(s_dump_done, pdMS_TO_TICKS(20000)) != pdTRUE) {
        ESP_LOGW(TAG, "dump timeout");
        return SKY_NUM_BLOCKS;
    }
    return s_dump_missing;
}

esp_err_t portal_host_init(void) {
    s_report_queue = xQueueCreate(REPORT_QUEUE_LEN, SKY_REPORT_LEN);
    s_dump_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_report_queue && s_dump_done, ESP_ERR_NO_MEM, TAG, "alloc");

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_RETURN_ON_ERROR(usb_host_install(&host_config), TAG, "usb_host_install");
    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL, 0);

    const hid_host_driver_config_t hid_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 6144,
        .core_id = 0,
        .callback = device_cb,
        .callback_arg = NULL,
    };
    ESP_RETURN_ON_ERROR(hid_host_install(&hid_config), TAG, "hid_host_install");

    xTaskCreatePinnedToCore(portal_task, "portal", 6144, NULL, 6, NULL, 1);
    ESP_LOGI(TAG, "portal host initialised; waiting for a portal…");
    return ESP_OK;
}
