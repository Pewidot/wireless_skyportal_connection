/*
 * link_protocol.h — ESP-NOW message format between the two ESP32-S3 boards.
 *
 * portal-side (N16R8, USB host) <----ESP-NOW----> console-side (T-Dongle, USB device)
 *
 * ESP-NOW frames carry <= 250 bytes of payload; every message below fits. All
 * structs are packed and little-endian (both peers are ESP32-S3). Every message
 * starts with link_hdr_t so the receiver can dispatch on `type`.
 *
 * Flow:
 *   portal -> console:  HELLO/HEALTH, PRESENCE, IMG_BEGIN/IMG_CHUNK/IMG_END
 *   console -> portal:  HELLO/HEALTH, WRITE_BEGIN/WRITE_BLOCK/WRITE_COMMIT
 *   either way:         ACK (with the seq being acknowledged)
 */
#ifndef LINK_PROTOCOL_H
#define LINK_PROTOCOL_H

#include <stdint.h>
#include "sky_protocol.h"

#define LINK_MAGIC        0x4B53      /* 'SK' */
#define LINK_VERSION      1
#define LINK_MAX_PAYLOAD  250
#define LINK_CHUNK_DATA   200         /* image bytes per IMG_CHUNK            */

/* Message types */
#define LINK_T_HELLO        0x01      /* discovery / heartbeat               */
#define LINK_T_HEALTH       0x02      /* link state + rssi + battery         */
#define LINK_T_PRESENCE     0x10      /* which slots occupied + toy ids      */
#define LINK_T_IMG_BEGIN    0x11      /* start of a 1 KB figure image        */
#define LINK_T_IMG_CHUNK    0x12      /* a slice of the image                */
#define LINK_T_IMG_END      0x13      /* image complete                      */
#define LINK_T_WRITE_BEGIN  0x20      /* console wants to write `count` blks  */
#define LINK_T_WRITE_BLOCK  0x21      /* one 16-byte block to write          */
#define LINK_T_WRITE_COMMIT 0x22      /* apply the buffered write set        */
#define LINK_T_ACK          0x30      /* positive ack of a seq               */
#define LINK_T_NACK         0x31      /* negative ack (status = reason)      */

/* v2 transparent tunnel: raw HID reports forwarded verbatim in both directions */
#define LINK_T_TUN_IN       0x40      /* portal -> console (interrupt-IN report) */
#define LINK_T_TUN_OUT      0x41      /* console -> portal (command / OUT data)  */
#define LINK_T_TUN_WRITE    0x42      /* console -> portal WRITE, needs ACK (hdr.seq) */
#define LINK_T_TUN_OUTEP    0x43      /* console -> portal interrupt-OUT EP (audio/LED) */

/* Roles (in HELLO) */
#define LINK_ROLE_PORTAL    0x01      /* N16R8, USB host                     */
#define LINK_ROLE_CONSOLE   0x02      /* T-Dongle, USB device                */

/* Link states (for HEALTH + the T-Dongle display) */
#define LINK_STATE_SCANNING 0x00
#define LINK_STATE_LINKED   0x01
#define LINK_STATE_BUSY     0x02      /* image transfer / write in progress  */

/* ACK / NACK status codes */
#define LINK_OK             0x00
#define LINK_ERR_CRC        0x01
#define LINK_ERR_SEQ        0x02
#define LINK_ERR_VERIFY     0x03      /* read-back after write mismatched    */
#define LINK_ERR_LOWPOWER   0x04      /* brown-out guard refused the write   */
#define LINK_ERR_NOFIGURE   0x05

typedef struct __attribute__((packed)) {
    uint16_t magic;     /* LINK_MAGIC   */
    uint8_t  version;   /* LINK_VERSION */
    uint8_t  type;      /* LINK_T_*     */
    uint16_t seq;       /* sender's monotonically increasing sequence */
} link_hdr_t;

typedef struct __attribute__((packed)) {
    link_hdr_t hdr;
    uint8_t  role;      /* LINK_ROLE_*           */
    uint8_t  fw_major;
    uint8_t  fw_minor;
} link_hello_t;

typedef struct __attribute__((packed)) {
    link_hdr_t hdr;
    uint8_t  state;     /* LINK_STATE_*          */
    int8_t   rssi;      /* last seen RSSI (dBm)  */
    uint8_t  battery;   /* 0..100, 0xFF = unknown */
} link_health_t;

/* One present figure (slot + toy id). */
typedef struct __attribute__((packed)) {
    uint8_t  slot;
    uint16_t toy_id;
} link_fig_t;

typedef struct __attribute__((packed)) {
    link_hdr_t hdr;
    uint16_t   present_mask;             /* bit i = slot i occupied */
    uint8_t    count;
    link_fig_t figs[SKY_MAX_SLOTS];
} link_presence_t;

typedef struct __attribute__((packed)) {
    link_hdr_t hdr;
    uint8_t  slot;
    uint16_t toy_id;
    uint16_t total_len;     /* normally SKY_DUMP_SIZE (1024) */
    uint16_t crc16;         /* sky_crc16 over the whole image */
} link_img_begin_t;

typedef struct __attribute__((packed)) {
    link_hdr_t hdr;
    uint8_t  slot;
    uint16_t offset;        /* byte offset into the image */
    uint8_t  len;           /* <= LINK_CHUNK_DATA */
    uint8_t  data[LINK_CHUNK_DATA];
} link_img_chunk_t;

typedef struct __attribute__((packed)) {
    link_hdr_t hdr;
    uint8_t  slot;
} link_img_end_t;

typedef struct __attribute__((packed)) {
    link_hdr_t hdr;
    uint8_t  slot;
    uint8_t  count;         /* number of WRITE_BLOCK messages to expect */
} link_write_begin_t;

typedef struct __attribute__((packed)) {
    link_hdr_t hdr;
    uint8_t  slot;
    uint8_t  block;         /* 0..63 */
    uint8_t  data[SKY_BLOCK_SIZE];
} link_write_block_t;

typedef struct __attribute__((packed)) {
    link_hdr_t hdr;
    uint8_t  slot;
    uint16_t crc16;         /* crc over the concatenated written blocks */
} link_write_commit_t;

typedef struct __attribute__((packed)) {
    link_hdr_t hdr;
    uint16_t ack_seq;       /* the seq being (n)acked */
    uint8_t  status;        /* LINK_OK / LINK_ERR_*   */
} link_ack_t;

/* v2 tunnel frame: one raw HID report (≤64 bytes) forwarded verbatim. */
typedef struct __attribute__((packed)) {
    link_hdr_t hdr;
    uint8_t  len;
    uint8_t  data[200];
} link_tunnel_t;

static inline void link_hdr_init(link_hdr_t *h, uint8_t type, uint16_t seq) {
    h->magic = LINK_MAGIC;
    h->version = LINK_VERSION;
    h->type = type;
    h->seq = seq;
}
static inline int link_hdr_valid(const link_hdr_t *h) {
    return h->magic == LINK_MAGIC && h->version == LINK_VERSION;
}

/* Number of IMG_CHUNK messages needed for a `total_len`-byte image. */
static inline int link_img_num_chunks(int total_len) {
    return (total_len + LINK_CHUNK_DATA - 1) / LINK_CHUNK_DATA;
}

#endif /* LINK_PROTOCOL_H */
