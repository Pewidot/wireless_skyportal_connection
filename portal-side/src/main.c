/*
 * main.c — v2 TUNNEL + write-safety, portal side (N16R8).
 *
 * Transparent bridge: forwards every IN report to the console and every command
 * to the real portal. On top it adds the figure-safety net:
 *   - snoops the figure data as the console reads it (per slot, up to 16 tags —
 *     so multiple figures, swapper halves and Trap Team traps are all covered);
 *   - on the first WRITE to a figure, saves a pre-write backup to NVS (keyed by
 *     the tag UID);
 *   - acknowledges each WRITE so the console can retry on packet loss;
 *   - BOOT button restores the backup of the figure currently on the portal.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "nvs.h"

#include "tunnel_host.h"
#include "esp_now_link.h"
#include "link_protocol.h"
#include "sky_protocol.h"
#include "led.h"

static const char *TAG = "main";
#define BTN_GPIO 0                         /* BOOT button = restore */

static volatile int64_t s_last_write_ms;   /* for the write-activity LED blink */
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* LED: blinks white while writing, dim green when linked & ready, red otherwise. */
static void led_task(void *arg) {
    led_init();
    bool blink = false;
    for (;;) {
        if (now_ms() - s_last_write_ms < 300) {        /* recent write → blink */
            blink = !blink;
            led_set(blink ? 90 : 0, blink ? 90 : 0, blink ? 90 : 0);
            vTaskDelay(pdMS_TO_TICKS(60));
        } else {
            if (link_is_linked() && tunnel_host_connected()) led_set(0, 30, 0);  /* ready */
            else if (link_is_linked())                       led_set(0, 0, 40);  /* linked, no portal */
            else                                             led_set(40, 0, 0);  /* no link */
            vTaskDelay(pdMS_TO_TICKS(120));
        }
    }
}

static uint8_t  s_snoop[SKY_MAX_SLOTS][SKY_DUMP_SIZE];
static bool     s_backed_up[SKY_MAX_SLOTS];
static volatile uint16_t s_present_mask;

static void uid_key(int slot, char *out) {  /* "bk_AABBCCDD" from block 0 */
    const uint8_t *u = s_snoop[slot];
    sprintf(out, "bk_%02X%02X%02X%02X", u[0], u[1], u[2], u[3]);
}

/* Pre-write backups run through a queue + task so the slow NVS commit NEVER
 * stalls the write/ACK path. A stall there delayed the ACK → the console
 * retried the write → the portal got hammered and the figure dropped off. */
typedef struct { uint8_t img[SKY_DUMP_SIZE]; } bak_req_t;
static QueueHandle_t s_bak_q;

static void request_backup(int slot) {
    if (!s_bak_q) return;
    static bak_req_t tmp;                  /* only ever touched from the rx worker */
    memcpy(tmp.img, s_snoop[slot], SKY_DUMP_SIZE);
    xQueueSend(s_bak_q, &tmp, 0);          /* snapshot now, commit to flash later */
}

static void backup_task(void *arg) {
    static bak_req_t req;
    for (;;) {
        if (xQueueReceive(s_bak_q, &req, portMAX_DELAY) != pdTRUE) continue;
        nvs_handle_t h;
        if (nvs_open("skybak", NVS_READWRITE, &h) != ESP_OK) continue;
        char key[16];
        const uint8_t *u = req.img;        /* UID = first 4 bytes of block 0 */
        sprintf(key, "bk_%02X%02X%02X%02X", u[0], u[1], u[2], u[3]);
        nvs_set_blob(h, key, req.img, SKY_DUMP_SIZE);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "pre-write backup saved (%s)", key);
    }
}

/* Build a live copy of each figure from the reads the console performs. */
static void snoop(const uint8_t *r, int len) {
    if (r[0] == SKY_CMD_QUERY && r[1] != 0x01 && len >= 19) {
        int slot = r[1] - 0x10, block = r[2];
        if (slot >= 0 && slot < SKY_MAX_SLOTS && block < SKY_NUM_BLOCKS)
            memcpy(s_snoop[slot] + block * SKY_BLOCK_SIZE, r + 3, SKY_BLOCK_SIZE);
    } else if (r[0] == SKY_CMD_STATUS && len >= 6) {
        uint32_t slots = r[1] | (r[2] << 8) | (r[3] << 16) | ((uint32_t)r[4] << 24);
        uint16_t mask = 0;
        for (int i = 0; i < SKY_MAX_SLOTS; i++) if ((slots >> (2 * i)) & 1) mask |= (uint16_t)(1u << i);
        s_present_mask = mask;
        for (int i = 0; i < SKY_MAX_SLOTS; i++)
            if (!(mask & (1u << i))) s_backed_up[i] = false;   /* tag gone → allow a fresh backup */
    }
}

/* portal -> console: forward + snoop */
static void on_portal_in(const uint8_t *report, int len) {
    snoop(report, len);
    link_tunnel_t m;
    link_hdr_init(&m.hdr, LINK_T_TUN_IN, 0);
    m.len = (uint8_t)(len > (int)sizeof(m.data) ? (int)sizeof(m.data) : len);
    memcpy(m.data, report, m.len);
    link_send(&m, sizeof(link_hdr_t) + 1 + m.len);
}

/* console -> portal */
static void on_link_recv(uint8_t type, const uint8_t *data, int len, int rssi) {
    if (type == LINK_T_TUN_OUT) {
        const link_tunnel_t *m = (const link_tunnel_t *)data;
        tunnel_host_send(m->data, m->len);
    } else if (type == LINK_T_TUN_OUTEP) {
        const link_tunnel_t *m = (const link_tunnel_t *)data;
        tunnel_host_send_out(m->data, m->len);   /* audio / trap-LED → OUT endpoint */
    } else if (type == LINK_T_TUN_WRITE) {
        const link_tunnel_t *m = (const link_tunnel_t *)data;
        s_last_write_ms = now_ms();          /* drive the write-activity LED */
        if (m->len >= 2) {
            int slot = m->data[1] & 0x0F;        /* WRITE target slot */
            if (slot < SKY_MAX_SLOTS && !s_backed_up[slot] && (s_present_mask & (1u << slot))) {
                request_backup(slot);            /* async — does not stall the write */
                s_backed_up[slot] = true;
            }
        }
        tunnel_host_send(m->data, m->len);       /* deliver to the figure (sync) */
        link_ack_t a;                            /* tell the console it arrived */
        link_hdr_init(&a.hdr, LINK_T_ACK, 0);
        a.ack_seq = m->hdr.seq;
        a.status = LINK_OK;
        link_send(&a, sizeof(a));
    }
}

/* Restore the backup of the figure currently on the portal. */
static void do_restore(void) {
    int slot = -1;
    for (int i = 0; i < SKY_MAX_SLOTS; i++) if (s_present_mask & (1u << i)) { slot = i; break; }
    if (slot < 0) { ESP_LOGW(TAG, "restore: no figure on portal"); return; }

    nvs_handle_t h;
    if (nvs_open("skybak", NVS_READONLY, &h) != ESP_OK) { ESP_LOGW(TAG, "restore: no nvs"); return; }
    char key[16];
    uid_key(slot, key);
    static uint8_t bak[SKY_DUMP_SIZE];
    size_t sz = SKY_DUMP_SIZE;
    esp_err_t e = nvs_get_blob(h, key, bak, &sz);
    nvs_close(h);
    if (e != ESP_OK) { ESP_LOGW(TAG, "restore: no backup for %s", key); return; }

    ESP_LOGI(TAG, "restoring %s …", key);
    for (int b = 1; b < SKY_NUM_BLOCKS; b++) {   /* skip block 0 (read-only UID) */
        uint8_t cmd[SKY_REPORT_LEN];
        sky_build_write(cmd, (uint8_t)slot, (uint8_t)b, bak + b * SKY_BLOCK_SIZE);
        tunnel_host_send(cmd, 19);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "restore done");
}

static void button_task(void *arg) {
    gpio_config_t io = { .pin_bit_mask = 1ULL << BTN_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = 1 };
    gpio_config(&io);
    bool prev = true;
    for (;;) {
        bool now = gpio_get_level(BTN_GPIO);
        if (prev && !now) {                       /* press */
            vTaskDelay(pdMS_TO_TICKS(50));
            if (!gpio_get_level(BTN_GPIO)) do_restore();
        }
        prev = now;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Wireless Portal v2 (TUNNEL+safe) — portal side");
    s_bak_q = xQueueCreate(4, sizeof(bak_req_t));
    tunnel_host_init(on_portal_in);
    link_init(LINK_ROLE_PORTAL, on_link_recv);
    xTaskCreate(backup_task, "backup", 4096, NULL, 2, NULL);
    xTaskCreate(button_task, "btn", 4096, NULL, 3, NULL);
    xTaskCreate(led_task, "led", 3072, NULL, 3, NULL);
}
