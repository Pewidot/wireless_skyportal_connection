/*
 * tunnel_host.c — v2: USB host to the real portal, forward-only.
 * Every interrupt-IN report is handed to the callback (to ship over ESP-NOW);
 * commands coming back from the console are sent to the portal via SET_REPORT.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "tunnel_host.h"

#define HID_REPORT_OUTPUT 2
static const char *TAG = "tun-host";

static volatile hid_host_device_handle_t s_dev;
static volatile bool s_connected;
static tunnel_in_cb  s_on_in;

static void iface_cb(hid_host_device_handle_t dev,
                     const hid_host_interface_event_t event, void *arg) {
    uint8_t data[64];
    size_t len = 0;
    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        if (hid_host_device_get_raw_input_report_data(dev, data, sizeof(data), &len) == ESP_OK
                && len > 0 && s_on_in)
            s_on_in(data, (int)len);
        break;
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        s_connected = false; s_dev = NULL;
        hid_host_device_close(dev);
        ESP_LOGI(TAG, "portal disconnected");
        break;
    default: break;
    }
}

static void device_cb(hid_host_device_handle_t dev,
                      const hid_host_driver_event_t event, void *arg) {
    if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
        const hid_host_device_config_t cfg = { .callback = iface_cb, .callback_arg = NULL };
        if (hid_host_device_open(dev, &cfg) != ESP_OK) return;
        if (hid_host_device_start(dev) != ESP_OK) return;
        s_dev = dev; s_connected = true;
        ESP_LOGI(TAG, "portal connected (tunnel)");
    }
}

static void usb_lib_task(void *arg) {
    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

void tunnel_host_init(tunnel_in_cb on_in) {
    s_on_in = on_in;
    const usb_host_config_t hc = { .skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    ESP_ERROR_CHECK(usb_host_install(&hc));
    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL, 0);
    const hid_host_driver_config_t hcfg = {
        .create_background_task = true, .task_priority = 5,
        .stack_size = 6144, .core_id = 0, .callback = device_cb, .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(hid_host_install(&hcfg));
    ESP_LOGI(TAG, "tunnel host up; waiting for portal");
}

bool tunnel_host_connected(void) { return s_connected; }

esp_err_t tunnel_host_send(const uint8_t *buf, int len) {
    if (!s_dev) return ESP_FAIL;
    return hid_class_request_set_report((hid_host_device_handle_t)s_dev,
                                        HID_REPORT_OUTPUT, 0, (uint8_t *)buf, len);
}

esp_err_t tunnel_host_send_out(const uint8_t *buf, int len) {
    if (!s_dev) return ESP_FAIL;
    return hid_host_device_output_report((hid_host_device_handle_t)s_dev, 0x02, buf, (size_t)len);
}
