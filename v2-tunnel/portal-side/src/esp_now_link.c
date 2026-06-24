/* esp_now_link.c — see esp_now_link.h. */
#include "esp_now_link.h"
#include "link_protocol.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"

#define LINK_CHANNEL   1
#define LINK_LOST_MS   3000

static const char *TAG = "link";
static const uint8_t BROADCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static uint8_t              s_role;
static link_recv_handler_t  s_handler;
static volatile bool        s_linked;
static volatile int         s_rssi;
static uint8_t              s_peer[6];
static volatile int64_t     s_last_rx_ms;
static uint16_t             s_seq;

/* Decouple the handler from the ESP-NOW RX callback: the callback runs in the
 * WiFi task and MUST NOT block, but the portal-side handler does synchronous
 * USB control transfers. So the callback only copies each frame into this queue
 * and a worker task runs the (possibly blocking) handler. */
typedef struct { int len; int rssi; uint8_t data[250]; } rx_item_t;
static QueueHandle_t s_rx_q;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void add_peer(const uint8_t mac[6]) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t p = { .channel = LINK_CHANNEL, .ifidx = WIFI_IF_STA, .encrypt = false };
    memcpy(p.peer_addr, mac, 6);
    esp_now_add_peer(&p);
}

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len < (int)sizeof(link_hdr_t)) return;
    const link_hdr_t *h = (const link_hdr_t *)data;
    if (!link_hdr_valid(h)) return;

    s_last_rx_ms = now_ms();
    if (info->rx_ctrl) s_rssi = info->rx_ctrl->rssi;

    /* Link bookkeeping is fast & non-blocking → keep it here so link state and
     * RSSI stay live even while the worker is busy doing USB I/O. */
    if (h->type == LINK_T_HELLO && len >= (int)sizeof(link_hello_t)) {
        const link_hello_t *hi = (const link_hello_t *)data;
        if (hi->role != s_role) {                       /* the other board */
            if (!s_linked || memcmp(s_peer, info->src_addr, 6) != 0) {
                memcpy(s_peer, info->src_addr, 6);
                add_peer(s_peer);
                s_linked = true;
                ESP_LOGI(TAG, "linked to %02x:%02x:%02x:%02x:%02x:%02x",
                         s_peer[0], s_peer[1], s_peer[2], s_peer[3], s_peer[4], s_peer[5]);
            }
        }
    }

    /* Hand everything else off to the worker task; never block in here. */
    if (s_handler && s_rx_q && len <= (int)sizeof(((rx_item_t *)0)->data)) {
        rx_item_t it;
        it.len = len;
        it.rssi = s_rssi;
        memcpy(it.data, data, len);
        if (xQueueSend(s_rx_q, &it, 0) != pdTRUE) {     /* full → drop oldest, keep order */
            rx_item_t drop;
            xQueueReceive(s_rx_q, &drop, 0);
            xQueueSend(s_rx_q, &it, 0);
        }
    }
}

static void rx_task(void *arg) {
    rx_item_t it;
    for (;;) {
        if (xQueueReceive(s_rx_q, &it, portMAX_DELAY) != pdTRUE) continue;
        const link_hdr_t *h = (const link_hdr_t *)it.data;
        if (s_handler) s_handler(h->type, it.data, it.len, it.rssi);
    }
}

static void link_task(void *arg) {
    for (;;) {
        if (s_linked && now_ms() - s_last_rx_ms > LINK_LOST_MS) {
            s_linked = false;
            ESP_LOGW(TAG, "peer lost, back to discovery");
        }
        link_hello_t hello;
        link_hdr_init(&hello.hdr, LINK_T_HELLO, s_seq++);
        hello.role = s_role;
        hello.fw_major = 1;
        hello.fw_minor = 0;
        if (s_linked) esp_now_send(s_peer, (uint8_t *)&hello, sizeof(hello));     /* keepalive */
        else          esp_now_send(BROADCAST, (uint8_t *)&hello, sizeof(hello));  /* discovery */
        vTaskDelay(pdMS_TO_TICKS(s_linked ? 1000 : 400));
    }
}

void link_init(uint8_t role, link_recv_handler_t handler) {
    s_role = role;
    s_handler = handler;

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(LINK_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    s_rx_q = xQueueCreate(32, sizeof(rx_item_t));
    xTaskCreate(rx_task, "link_rx", 8192, NULL, 6, NULL);
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    add_peer(BROADCAST);

    xTaskCreate(link_task, "link", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "ESP-NOW link up (role %d), discovering…", role);
}

bool link_is_linked(void) { return s_linked; }
int  link_rssi(void)      { return s_rssi; }

esp_err_t link_send(const void *data, size_t len) {
    if (!s_linked) return ESP_FAIL;
    return esp_now_send(s_peer, (const uint8_t *)data, len);
}
