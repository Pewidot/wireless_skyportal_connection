/*
 * main.c — M2/M3 console-side: emulate a Portal of Power over native USB,
 * with a live text debug log on the T-Dongle's ST7735.
 *
 * Screen layout (8x8 font, 20x10):
 *   WIRELESS PORTAL          (title)
 *   LINK  OK -45dB           (ESP-NOW link to the N16R8)
 *   USB   READING            (console / PC talking to us)
 *   FIG   16                 (toy-id currently emulated)
 *   <rolling event log…>
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "tinyusb.h"

#include "usb_descriptors.h"
#include "portal_device.h"
#include "display.h"
#include "esp_now_link.h"
#include "link_protocol.h"
#include "sky_protocol.h"

static const char *TAG = "main";

/* ---- incoming relay: figure presence + 1 KB images from the N16R8 ---- */
static uint8_t s_rx_img[SKY_DUMP_SIZE];

static void dlog(const char *fmt, ...);   /* fwd */

static void on_link_recv(uint8_t type, const uint8_t *data, int len, int rssi) {
    switch (type) {
    case LINK_T_PRESENCE: {
        if (len < (int)(sizeof(link_hdr_t) + 3)) return;
        const link_presence_t *m = (const link_presence_t *)data;
        portal_device_sync_mask(m->present_mask);
        int cnt = m->count <= SKY_MAX_SLOTS ? m->count : SKY_MAX_SLOTS;
        for (int i = 0; i < cnt; i++)
            portal_device_set_present(m->figs[i].slot, m->figs[i].toy_id);
        break;
    }
    case LINK_T_IMG_BEGIN:
        memset(s_rx_img, 0, sizeof(s_rx_img));
        break;
    case LINK_T_IMG_CHUNK: {
        const link_img_chunk_t *c = (const link_img_chunk_t *)data;
        if ((int)c->offset + c->len <= SKY_DUMP_SIZE)
            memcpy(s_rx_img + c->offset, c->data, c->len);
        break;
    }
    case LINK_T_IMG_END: {
        const link_img_end_t *e = (const link_img_end_t *)data;
        portal_device_set_image(e->slot, s_rx_img);
        dlog("img slot%u rx", e->slot);
        break;
    }
    }
}

#define BLACK   RGB565(0, 0, 0)
#define WHITE   RGB565(255, 255, 255)
#define YELLOW  RGB565(255, 255, 0)
#define GREEN   RGB565(0, 255, 0)
#define RED     RGB565(255, 80, 80)
#define CYAN    RGB565(0, 255, 255)
#define GREY    RGB565(150, 150, 150)

/* ---- rolling debug log ---- */
#define LOG_LINES 6
static char s_log[LOG_LINES][LCD_COLS + 1];
static int  s_log_head;

static void dlog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_log[s_log_head], LCD_COLS + 1, fmt, ap);
    va_end(ap);
    s_log_head = (s_log_head + 1) % LOG_LINES;
    ESP_LOGI(TAG, "%s", s_log[(s_log_head + LOG_LINES - 1) % LOG_LINES]);
}

static void draw_line(int row, const char *text, uint16_t fg) {
    char padded[LCD_COLS + 1];
    snprintf(padded, sizeof(padded), "%-20.20s", text);
    display_text(0, row * 8, padded, fg, BLACK);
}

static void display_task(void *arg) {
    bool p_link = false, p_mnt = false, p_act = false;
    uint16_t p_toy = 0xFFFF;
    char buf[24];
    for (;;) {
        bool link = link_is_linked();
        bool mnt  = tud_mounted();
        bool act  = portal_device_activated();
        uint16_t toy = portal_device_current_toy();

        if (link != p_link) { dlog(link ? "link up %ddB" : "link lost", link_rssi()); p_link = link; }
        if (mnt  != p_mnt)  { dlog(mnt  ? "host connected" : "host gone");             p_mnt  = mnt;  }
        if (act  != p_act)  { dlog(act  ? "host ACTIVATE" : "host idle");              p_act  = act;  }
        if (toy  != p_toy)  { if (toy) dlog("figure id=%u", toy); else dlog("no figure"); p_toy = toy; }

        draw_line(0, "WIRELESS PORTAL", YELLOW);
        snprintf(buf, sizeof(buf), "LINK  %s %ddB", link ? "OK" : "--", link_rssi());
        draw_line(1, buf, link ? GREEN : RED);
        snprintf(buf, sizeof(buf), "USB   %s", mnt ? (act ? "READING" : "READY") : "----");
        draw_line(2, buf, mnt ? CYAN : GREY);
        snprintf(buf, sizeof(buf), "FIG   %u", toy);
        draw_line(3, buf, toy ? WHITE : GREY);

        for (int i = 0; i < LOG_LINES; i++) {
            int idx = (s_log_head + i) % LOG_LINES;   /* oldest first */
            draw_line(4 + i, s_log[idx], GREY);
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Wireless Portal — console side");
    display_init();
    display_fill(BLACK);
    dlog("booting...");

    portal_device_init_figure();

    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = &portal_device_desc,
        .string_descriptor = portal_string_desc,
        .string_descriptor_count = portal_string_desc_count,
        .external_phy = false,
        .configuration_descriptor = portal_config_desc,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    dlog("USB portal up");

    portal_device_start();
    xTaskCreate(display_task, "display", 4096, NULL, 3, NULL);
    link_init(LINK_ROLE_CONSOLE, on_link_recv);
    dlog("ESP-NOW scan...");
}
