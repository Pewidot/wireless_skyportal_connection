/*
 * main.c — v2 TUNNEL + write-safety + display, console side (T-Dongle).
 * Transparent bridge to the real portal with reliable WRITEs, plus the v1 text
 * status display (it snoops the pass-through stream just to show status).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tinyusb.h"

#include "usb_descriptors.h"
#include "esp_now_link.h"
#include "link_protocol.h"
#include "sky_protocol.h"
#include "display.h"

static const char *TAG = "main";

#define BLACK  RGB565(0,0,0)
#define WHITE  RGB565(255,255,255)
#define YELLOW RGB565(255,255,0)
#define GREEN  RGB565(0,255,0)
#define RED    RGB565(255,80,80)
#define CYAN   RGB565(0,255,255)
#define GREY   RGB565(150,150,150)

typedef struct { uint8_t len; uint8_t data[64]; } item_t;
static QueueHandle_t s_in_q;
static QueueHandle_t s_write_q;
static SemaphoreHandle_t s_ack_sem;
static volatile uint16_t s_acked_seq;

/* snooped tunnel status (for the display only) */
static volatile int      s_present_count;
static volatile uint16_t s_toy;
static volatile uint32_t s_writes;
static volatile uint32_t s_sound_pkts;     /* audio packets seen on the OUT endpoint */
static volatile int64_t  s_last_write_ms;
/* The console only wants the live status stream AFTER it has activated the
 * portal. A real portal stays quiet until then; we mirror that so the PS4 can
 * finish detection without the stream flooding the endpoint mid-handshake. */
static volatile bool     s_console_active;
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* Cached real handshake replies, learned from the tunnel so the dongle can
 * answer the console instantly yet byte-accurately. Defaults match a Trap Team
 * (Traptanium) portal until the real portal's replies are observed. */
static uint8_t s_ready_resp[64] = { SKY_CMD_READY, 0x02, 0x18 };
static uint8_t s_ready_len = 32;
static uint8_t s_act_resp[64]  = { SKY_CMD_ACTIVATE, 0x01, 0xFF, 0x77 };
static uint8_t s_act_len  = 32;

/* Push a report to the console, dropping the OLDEST queued item if full — so a
 * fresh status update (e.g. a figure/trap REMOVAL) is never starved out by a
 * backlog. Losing the trap-removal frame is exactly what left it stuck "on". */
static void push_in(const item_t *it) {
    if (xQueueSend(s_in_q, it, 0) != pdTRUE) {
        item_t drop;
        xQueueReceive(s_in_q, &drop, 0);
        xQueueSend(s_in_q, it, 0);
    }
}

static void queue_local(const uint8_t *d, int len) {
    item_t it;
    it.len = (uint8_t)(len > (int)sizeof(it.data) ? (int)sizeof(it.data) : len);
    memcpy(it.data, d, it.len);
    push_in(&it);
}

/* ---- debug log ---- */
#define LOG_LINES 5
static char s_log[LOG_LINES][LCD_COLS + 1];
static int  s_log_head;
static void dlog(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(s_log[s_log_head], LCD_COLS + 1, fmt, ap);
    va_end(ap);
    s_log_head = (s_log_head + 1) % LOG_LINES;
}

static void snoop_in(const uint8_t *r, int len) {
    if (r[0] == SKY_CMD_STATUS && len >= 5) {
        uint32_t slots = r[1] | (r[2] << 8) | (r[3] << 16) | ((uint32_t)r[4] << 24);
        int c = 0;
        for (int i = 0; i < SKY_MAX_SLOTS; i++) if ((slots >> (2 * i)) & 1) c++;
        s_present_count = c;
    } else if (r[0] == SKY_CMD_QUERY && r[1] != 0x01 && r[2] == SKY_BLOCK_TOY_ID && len >= 5) {
        s_toy = r[3] | (r[4] << 8);
    }
}

/* portal -> console + ACKs */
static void on_link_recv(uint8_t type, const uint8_t *data, int len, int rssi) {
    if (type == LINK_T_TUN_IN && len >= (int)(sizeof(link_hdr_t) + 1)) {
        const link_tunnel_t *m = (const link_tunnel_t *)data;
        int n = m->len > sizeof(s_ready_resp) ? sizeof(s_ready_resp) : m->len;
        if (n >= 1 && m->data[0] == SKY_CMD_READY) {
            memcpy(s_ready_resp, m->data, n); s_ready_len = (uint8_t)n;  /* learn + swallow */
        } else if (n >= 1 && m->data[0] == SKY_CMD_ACTIVATE) {
            memcpy(s_act_resp, m->data, n); s_act_len = (uint8_t)n;      /* learn + swallow */
        } else {
            item_t it;
            it.len = (uint8_t)n;
            memcpy(it.data, m->data, it.len);
            snoop_in(it.data, it.len);                 /* always track for display */
            /* Hold back the unsolicited status stream until the console has
             * activated us, otherwise it floods the PS4 during detection. */
            if (!(it.data[0] == SKY_CMD_STATUS && !s_console_active))
                push_in(&it);
        }
    } else if (type == LINK_T_ACK && len >= (int)sizeof(link_ack_t)) {
        const link_ack_t *a = (const link_ack_t *)data;
        s_acked_seq = a->ack_seq;
        xSemaphoreGive(s_ack_sem);
    }
}

static void tx_task(void *arg) {
    item_t it;
    for (;;) {
        if (xQueueReceive(s_in_q, &it, portMAX_DELAY) != pdTRUE) continue;
        /* Wait for the IN endpoint to be free, then send; retry a few times if
         * the report didn't take so an update isn't silently dropped. */
        for (int attempt = 0; attempt < 3; attempt++) {
            int tries = 0;
            while (!(tud_mounted() && tud_hid_ready()) && tries++ < 50) vTaskDelay(pdMS_TO_TICKS(1));
            if (!tud_mounted()) break;
            if (tud_hid_report(0, it.data, it.len)) break;
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
}

static void write_task(void *arg) {
    static uint16_t seq = 1;
    item_t w;
    for (;;) {
        if (xQueueReceive(s_write_q, &w, portMAX_DELAY) != pdTRUE) continue;
        uint16_t myseq = seq++;
        bool acked = false;
        for (int attempt = 0; attempt < 6 && !acked; attempt++) {
            link_tunnel_t m;
            link_hdr_init(&m.hdr, LINK_T_TUN_WRITE, myseq);
            m.len = w.len; memcpy(m.data, w.data, w.len);
            xSemaphoreTake(s_ack_sem, 0);
            link_send(&m, sizeof(link_hdr_t) + 1 + m.len);
            /* 100 ms: tolerate a momentarily busy worker (sound burst) without
             * firing spurious retries that would hammer the portal. */
            if (xSemaphoreTake(s_ack_sem, pdMS_TO_TICKS(100)) == pdTRUE && s_acked_seq == myseq) acked = true;
        }
        s_writes++;
        s_last_write_ms = now_ms();
        if (!acked) dlog("WRITE %u LOST!", myseq);
    }
}

static void draw_line(int row, const char *t, uint16_t fg) {
    char p[LCD_COLS + 1];
    snprintf(p, sizeof(p), "%-20.20s", t);
    display_text(0, row * 8, p, fg, BLACK);
}

static void display_task(void *arg) {
    bool p_link = false, p_mnt = false; int p_cnt = -1; uint16_t p_toy = 0xFFFF;
    char buf[24];
    for (;;) {
        bool link = link_is_linked();
        bool mnt  = tud_mounted();
        int  cnt  = s_present_count;
        uint16_t toy = s_toy;
        bool writing = (now_ms() - s_last_write_ms) < 400;

        if (link != p_link) { dlog(link ? "link up %ddB" : "link lost", link_rssi()); p_link = link; }
        if (mnt  != p_mnt)  { dlog(mnt ? "host connected" : "host gone"); p_mnt = mnt; }
        if (cnt  != p_cnt)  { dlog("figures: %d", cnt); p_cnt = cnt; }
        if (toy  != p_toy)  { if (toy) dlog("toy id=%u", toy); p_toy = toy; }

        draw_line(0, "WL PORTAL  TUNNEL", YELLOW);
        snprintf(buf, sizeof(buf), "LINK  %s %ddB", link ? "OK" : "--", link_rssi());
        draw_line(1, buf, link ? GREEN : RED);
        snprintf(buf, sizeof(buf), "USB   %s", mnt ? "CONNECTED" : "----");
        draw_line(2, buf, mnt ? CYAN : GREY);
        snprintf(buf, sizeof(buf), "FIG %u x%d %s", toy, cnt, writing ? "WRITE" : "");
        draw_line(3, buf, writing ? WHITE : (cnt ? WHITE : GREY));
        snprintf(buf, sizeof(buf), "W:%lu SND:%lu",
                 (unsigned long)s_writes, (unsigned long)s_sound_pkts);
        draw_line(4, buf, GREY);

        for (int i = 0; i < LOG_LINES; i++)
            draw_line(5 + i, s_log[(s_log_head + i) % LOG_LINES], GREY);

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Wireless Portal v2 (TUNNEL+safe+display) — console side");
    display_init();
    display_fill(BLACK);
    dlog("v2 tunnel boot");

    s_in_q = xQueueCreate(48, sizeof(item_t));
    s_write_q = xQueueCreate(16, sizeof(item_t));
    s_ack_sem = xSemaphoreCreateBinary();

    const tinyusb_config_t cfg = {
        .device_descriptor = &portal_device_desc,
        .string_descriptor = portal_string_desc,
        .string_descriptor_count = portal_string_desc_count,
        .external_phy = false,
        .configuration_descriptor = portal_config_desc,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));
    xTaskCreate(tx_task, "tun_tx", 4096, NULL, 5, NULL);
    xTaskCreate(write_task, "tun_wr", 4096, NULL, 6, NULL);
    xTaskCreate(display_task, "display", 4096, NULL, 3, NULL);
    link_init(LINK_ROLE_CONSOLE, on_link_recv);
}

/* ---- TinyUSB device callbacks ---- */
/* Start clean every time the console (re)connects: no stale stream, not yet
 * activated. This is what makes detection work even if the portal ESP was
 * already running and streaming when the dongle is plugged into the PS4. */
void tud_mount_cb(void)  { s_console_active = false; xQueueReset(s_in_q); }
void tud_umount_cb(void) { s_console_active = false; xQueueReset(s_in_q); }

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
    (void) instance; (void) report_type;
    /* Data from the interrupt OUT endpoint (tagged 0xEE by our TinyUSB patch) is
     * the portal's audio + trap-LED stream. It must go to the portal's OWN OUT
     * endpoint (0x02), NOT down the command path — sending it as control commands
     * made the portal mis-parse audio bytes (e.g. Activate-off → figure dark).
     * Forward it on its own link channel; the N16R8 delivers it to EP 0x02. */
    if (report_id == 0xEE) {
        s_sound_pkts++;
        link_tunnel_t m;
        link_hdr_init(&m.hdr, LINK_T_TUN_OUTEP, 0);
        m.len = (uint8_t)(bufsize > sizeof(m.data) ? sizeof(m.data) : bufsize);
        memcpy(m.data, buffer, m.len);
        link_send(&m, sizeof(link_hdr_t) + 1 + m.len);
        return;
    }
    if (bufsize < 1) return;
    /* Strictly-validated WRITE → reliable path + backup. Strict (slot nibble +
     * block range) so stray/audio bytes that merely start with 'W' can't be
     * mistaken for a tag write and corrupt a figure. */
    if (buffer[0] == SKY_CMD_WRITE && bufsize >= 19 &&
        (buffer[1] & 0xF0) == 0x10 && buffer[2] < SKY_NUM_BLOCKS) {
        item_t w;
        w.len = (uint8_t)(bufsize > sizeof(w.data) ? sizeof(w.data) : bufsize);
        memcpy(w.data, buffer, w.len);
        xQueueSend(s_write_q, &w, 0);
        return;
    }
    /* Answer the handshake commands locally & instantly so the console
     * recognises the portal even before the wireless round-trip completes —
     * this is what lets the PS4 detect it on a fresh boot (no hot-swap trick). */
    if (buffer[0] == SKY_CMD_READY) {
        queue_local(s_ready_resp, s_ready_len);
    } else if (buffer[0] == SKY_CMD_ACTIVATE) {
        if (bufsize >= 2) {
            s_act_resp[1] = buffer[1];                 /* echo requested on/off */
            s_console_active = (buffer[1] != 0);       /* gate the status stream */
        }
        queue_local(s_act_resp, s_act_len);
    }
    /* Still forward EVERYTHING (Ready/Activate/Color/sound/Query/...) to the
     * real portal so it stays in sync, sound & traps work, and we learn its
     * exact replies for next time. */
    link_tunnel_t m;
    link_hdr_init(&m.hdr, LINK_T_TUN_OUT, 0);
    m.len = (uint8_t)(bufsize > sizeof(m.data) ? sizeof(m.data) : bufsize);
    memcpy(m.data, buffer, m.len);
    link_send(&m, sizeof(link_hdr_t) + 1 + m.len);
}
