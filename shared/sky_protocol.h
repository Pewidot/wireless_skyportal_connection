/*
 * sky_protocol.h — Skylanders "Portal of Power" USB command protocol.
 *
 * Shared by both ESP32-S3 boards. This is the protocol we verified against real
 * hardware (VID 0x1430 / PID 0x0150): 32-byte reports, commands sent as USB
 * SET_REPORT (Output) control transfers, responses on the interrupt-IN endpoint.
 *
 * The portal handles all NFC / Mifare authentication internally, so this layer
 * never needs the figure encryption keys — it only frames commands and parses
 * the Status / Query responses.
 */
#ifndef SKY_PROTOCOL_H
#define SKY_PROTOCOL_H

#include <stdint.h>
#include <string.h>

/* ---- USB identity ------------------------------------------------------- */
#define SKY_VID            0x1430
#define SKY_PID            0x0150
#define SKY_EP_IN          0x81      /* interrupt IN  (portal -> host)        */
#define SKY_EP_OUT         0x01      /* interrupt OUT (unreliable; use ctrl)  */
#define SKY_REPORT_LEN     32        /* every report is 32 bytes              */

/* SET_REPORT control transfer used to SEND commands to the portal:
 *   bmRequestType 0x21, bRequest 0x09, wValue 0x0200 (Output, report id 0),
 *   wIndex 0 (interface), 32-byte payload.                                   */
#define SKY_SET_REPORT_REQTYPE 0x21
#define SKY_SET_REPORT_REQ     0x09
#define SKY_SET_REPORT_WVALUE  0x0200

/* ---- Command characters (first byte of an outgoing report) -------------- */
#define SKY_CMD_READY      0x52      /* 'R'  poll readiness; returns portal id */
#define SKY_CMD_ACTIVATE   0x41      /* 'A' 0x01 = on / 0x00 = off             */
#define SKY_CMD_COLOR      0x43      /* 'C' r g b                              */
#define SKY_CMD_STATUS     0x53      /* 'S'  (portal also streams this)        */
#define SKY_CMD_QUERY      0x51      /* 'Q' slot block   -> read 16-byte block */
#define SKY_CMD_WRITE      0x57      /* 'W' (0x10|slot) block <16 bytes>       */

/* ---- Figure layout ------------------------------------------------------ */
#define SKY_BLOCK_SIZE     16
#define SKY_NUM_BLOCKS     64
#define SKY_DUMP_SIZE      (SKY_BLOCK_SIZE * SKY_NUM_BLOCKS)  /* 1024 bytes    */
#define SKY_BLOCK_TOY_ID   0x01      /* block holding the toy/character id     */
#define SKY_MAX_SLOTS      16

/* ---- Command builders (write into a 32-byte buffer) --------------------- */
static inline void sky_cmd_clear(uint8_t buf[SKY_REPORT_LEN]) {
    memset(buf, 0, SKY_REPORT_LEN);
}
static inline void sky_build_ready(uint8_t buf[SKY_REPORT_LEN]) {
    sky_cmd_clear(buf); buf[0] = SKY_CMD_READY;
}
static inline void sky_build_activate(uint8_t buf[SKY_REPORT_LEN], int on) {
    sky_cmd_clear(buf); buf[0] = SKY_CMD_ACTIVATE; buf[1] = on ? 0x01 : 0x00;
}
static inline void sky_build_color(uint8_t buf[SKY_REPORT_LEN],
                                   uint8_t r, uint8_t g, uint8_t b) {
    sky_cmd_clear(buf); buf[0] = SKY_CMD_COLOR; buf[1] = r; buf[2] = g; buf[3] = b;
}
static inline void sky_build_query(uint8_t buf[SKY_REPORT_LEN],
                                   uint8_t slot, uint8_t block) {
    sky_cmd_clear(buf); buf[0] = SKY_CMD_QUERY; buf[1] = slot; buf[2] = block;
}
static inline void sky_build_write(uint8_t buf[SKY_REPORT_LEN], uint8_t slot,
                                   uint8_t block, const uint8_t data[16]) {
    sky_cmd_clear(buf);
    buf[0] = SKY_CMD_WRITE; buf[1] = (uint8_t)(0x10 | (slot & 0x0F)); buf[2] = block;
    memcpy(&buf[3], data, 16);
}

/* ---- Response parsing ---------------------------------------------------- */

/* Status report: byte0 == 'S', bytes 1..4 = little-endian u32 of 16 x 2-bit
 * slot states (bit0 = present, bit1 = changed), byte5 = sequence counter. */
static inline uint32_t sky_status_slots(const uint8_t *r) {
    return (uint32_t)r[1] | ((uint32_t)r[2] << 8) |
           ((uint32_t)r[3] << 16) | ((uint32_t)r[4] << 24);
}
static inline int sky_slot_present(uint32_t slots, int i) {
    return (slots >> (2 * i)) & 0x1;
}
static inline int sky_slot_changed(uint32_t slots, int i) {
    return (slots >> (2 * i)) & 0x2;
}
static inline uint16_t sky_present_mask(uint32_t slots) {
    uint16_t m = 0;
    for (int i = 0; i < SKY_MAX_SLOTS; i++)
        if (sky_slot_present(slots, i)) m |= (uint16_t)(1u << i);
    return m;
}

/* Query response: byte0 == 'Q', byte1 == 0x10+slot (0x01 = error),
 * byte2 == block, bytes 3..18 = the 16 data bytes. */
static inline int sky_is_query_resp(const uint8_t *r) { return r[0] == SKY_CMD_QUERY; }
static inline int sky_query_ok(const uint8_t *r)      { return r[1] != 0x01; }
static inline int sky_query_slot(const uint8_t *r)    { return r[1] - 0x10; }
static inline int sky_query_block(const uint8_t *r)   { return r[2]; }
static inline const uint8_t *sky_query_data(const uint8_t *r) { return &r[3]; }

/* Toy/character id from a dump (block 1, bytes 0-1, little-endian). */
static inline uint16_t sky_toy_id(const uint8_t dump[SKY_DUMP_SIZE]) {
    const uint8_t *b1 = &dump[SKY_BLOCK_TOY_ID * SKY_BLOCK_SIZE];
    return (uint16_t)b1[0] | ((uint16_t)b1[1] << 8);
}

/* ---- CRC16 (CCITT, poly 0x1021, init 0xFFFF) ----------------------------
 * Skylanders save areas carry a CRC16 over part of the figure. We use it only
 * for optional integrity checks during the safe-write read-back. */
static inline uint16_t sky_crc16(const uint8_t *data, int len) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

#endif /* SKY_PROTOCOL_H */
