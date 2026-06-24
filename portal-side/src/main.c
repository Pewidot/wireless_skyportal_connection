/*
 * main.c — M1 portal-side bring-up + onboard status LED.
 *
 * Hosts a real Portal of Power and prints, over UART:
 *   - which figure (toy-ID) is placed/removed,
 *   - a full 1 KB dump of each newly-placed figure.
 * The onboard WS2812 mirrors the state (orange=no portal, blue=ready, green=figure).
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "portal_host.h"
#include "status_led.h"
#include "sky_protocol.h"
#include "esp_now_link.h"
#include "link_protocol.h"

static const char *TAG = "main";
static QueueHandle_t s_present_q;          /* slots of newly-placed figures */
static volatile int  s_present_count;      /* figures currently on the portal */
static uint16_t      s_toy[SKY_MAX_SLOTS]; /* toy-id per slot, 0 = absent */

/* Send the current presence (which slots + toy-ids) to the dongle. */
static void send_presence(void) {
    link_presence_t m;
    link_hdr_init(&m.hdr, LINK_T_PRESENCE, 0);
    m.present_mask = 0;
    m.count = 0;
    for (int i = 0; i < SKY_MAX_SLOTS; i++) {
        if (s_toy[i]) {
            m.present_mask |= (uint16_t)(1u << i);
            m.figs[m.count].slot = i;
            m.figs[m.count].toy_id = s_toy[i];
            m.count++;
        }
    }
    link_send(&m, sizeof(link_hdr_t) + 3 + m.count * sizeof(link_fig_t));
}

/* Stream a figure's full 1 KB image to the dongle. */
static void send_image(int slot, uint16_t toy, const uint8_t *dump) {
    link_img_begin_t b;
    link_hdr_init(&b.hdr, LINK_T_IMG_BEGIN, 0);
    b.slot = slot; b.toy_id = toy; b.total_len = SKY_DUMP_SIZE;
    b.crc16 = sky_crc16(dump, SKY_DUMP_SIZE);
    link_send(&b, sizeof(b));

    int chunks = link_img_num_chunks(SKY_DUMP_SIZE);
    for (int c = 0; c < chunks; c++) {
        link_img_chunk_t ch;
        link_hdr_init(&ch.hdr, LINK_T_IMG_CHUNK, c);
        ch.slot = slot;
        ch.offset = c * LINK_CHUNK_DATA;
        int nbytes = SKY_DUMP_SIZE - ch.offset;
        if (nbytes > LINK_CHUNK_DATA) nbytes = LINK_CHUNK_DATA;
        ch.len = nbytes;
        memcpy(ch.data, dump + ch.offset, nbytes);
        link_send(&ch, sizeof(link_hdr_t) + 4 + nbytes);
        vTaskDelay(pdMS_TO_TICKS(8));      /* pace the radio */
    }
    link_img_end_t e;
    link_hdr_init(&e.hdr, LINK_T_IMG_END, 0);
    e.slot = slot;
    link_send(&e, sizeof(e));
}

static void on_present(int slot, uint16_t toy_id) {
    ESP_LOGI(TAG, "FIGURE placed  slot=%d  toyID=%u (0x%04X)", slot, toy_id, toy_id);
    s_present_count++;
    if (slot >= 0 && slot < SKY_MAX_SLOTS) s_toy[slot] = toy_id;
    send_presence();
    xQueueSend(s_present_q, &slot, 0);      /* dump_task will relay the image */
}

static void on_remove(int slot) {
    ESP_LOGI(TAG, "FIGURE removed slot=%d", slot);
    if (s_present_count > 0) s_present_count--;
    if (slot >= 0 && slot < SKY_MAX_SLOTS) s_toy[slot] = 0;
    send_presence();
}

static void hexdump_row(const char *label, const uint8_t *p) {
    char line[64];
    int n = 0;
    for (int i = 0; i < SKY_BLOCK_SIZE; i++) n += sprintf(line + n, "%02X ", p[i]);
    ESP_LOGI(TAG, "  %s: %s", label, line);
}

static void dump_task(void *arg) {
    static uint8_t dump[SKY_DUMP_SIZE];
    int slot;
    for (;;) {
        if (xQueueReceive(s_present_q, &slot, portMAX_DELAY) != pdTRUE) continue;
        ESP_LOGI(TAG, "dumping slot %d…", slot);
        int missing = portal_host_dump(slot, dump);
        ESP_LOGI(TAG, "dump slot %d done (%d blocks missing), toyID=%u",
                 slot, missing, sky_toy_id(dump));
        hexdump_row("block0 (UID)", &dump[0]);
        hexdump_row("block1 (id) ", &dump[SKY_BLOCK_SIZE]);
        if (missing >= 0 && slot >= 0 && slot < SKY_MAX_SLOTS && s_toy[slot]) {
            send_image(slot, s_toy[slot], dump);   /* relay to the dongle */
            ESP_LOGI(TAG, "relayed slot %d image over ESP-NOW", slot);
        }
    }
}

/* Reflect portal state on the onboard LED. */
static void led_task(void *arg) {
    led_state_t last = -1;
    int tick = 0;
    for (;;) {
        led_state_t st = (s_present_count > 0)      ? LED_FIGURE
                       : portal_host_connected()    ? LED_PORTAL_READY
                                                    : LED_WAITING;
        if (st != last) { status_led_set(st); last = st; }
        if (++tick >= 7) { tick = 0; send_presence(); }  /* ~1 s: heal lost packets */
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Wireless Portal — portal side (M1)");
    status_led_init();

    s_present_q = xQueueCreate(8, sizeof(int));
    xTaskCreate(dump_task, "dump", 4096, NULL, 4, NULL);
    xTaskCreate(led_task, "led", 2560, NULL, 3, NULL);

    portal_host_set_callbacks(on_present, on_remove);
    ESP_ERROR_CHECK(portal_host_init());

    link_init(LINK_ROLE_PORTAL, NULL);   /* M3: ESP-NOW link to the dongle */
}
