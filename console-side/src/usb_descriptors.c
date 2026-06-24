/*
 * usb_descriptors.c — present the T-Dongle as a real Portal of Power.
 *
 * Same VID/PID (0x1430/0x0150), the same vendor HID report descriptor (32-byte
 * IN + OUT reports), and the same string identities the real portal reports, so
 * the console (or our PC test tool) cannot tell it apart from the hardware.
 */
#include "usb_descriptors.h"
#include "sky_protocol.h"

/* Exact copy of the Portal of Power's HID report descriptor (vendor page,
 * 32-byte input + output, no report id). */
const uint8_t portal_hid_report_desc[] = {
    0x06, 0x00, 0xFF,        /* Usage Page (Vendor 0xFF00) */
    0x09, 0x01,              /* Usage (0x01)               */
    0xA1, 0x01,              /* Collection (Application)   */
    0x19, 0x01,              /*   Usage Minimum (0x01)     */
    0x29, 0x40,              /*   Usage Maximum (0x40)     */
    0x15, 0x00,              /*   Logical Minimum (0)      */
    0x26, 0xFF, 0x00,        /*   Logical Maximum (255)    */
    0x75, 0x08,              /*   Report Size (8)          */
    0x95, 0x20,              /*   Report Count (32)        */
    0x81, 0x00,              /*   Input (Data,Array)       */
    0x19, 0x01,              /*   Usage Minimum (0x01)     */
    0x29, 0xFF,              /*   Usage Maximum (0xFF)     */ /* matches Trap Team portal */
    0x91, 0x00,              /*   Output (Data,Array)      */
    0xC0,                    /* End Collection             */
};

const tusb_desc_device_t portal_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = SKY_VID,
    .idProduct          = SKY_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

const uint8_t portal_config_desc[] = {
    /* config: 1 interface, value 1, no string, total len, attr, 100 mA */
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    /* HID in/out: itf 0, str 4, no boot protocol, report-desc len,
     * out ep 0x01, in ep 0x81, 32-byte reports, 1 ms interval */
    TUD_HID_INOUT_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_NONE,
                             sizeof(portal_hid_report_desc),
                             SKY_EP_OUT, SKY_EP_IN, SKY_REPORT_LEN, 1),
};

const char *portal_string_desc[] = {
    (const char[]){0x09, 0x04},   /* 0: language id (English)  */
    "Activision",                  /* 1: manufacturer           */
    "Spyro Porta",                 /* 2: product                */
    "0001",                        /* 3: serial                 */
    "Portal",                      /* 4: HID interface          */
};
const int portal_string_desc_count = 5;

/* TinyUSB asks for the report descriptor here. */
const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void) instance;
    return portal_hid_report_desc;
}
