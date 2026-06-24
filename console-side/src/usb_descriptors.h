/* usb_descriptors.h — emulated Portal of Power USB descriptors. */
#pragma once
#include "tusb.h"

extern const tusb_desc_device_t portal_device_desc;
extern const uint8_t  portal_config_desc[];
extern const uint8_t  portal_hid_report_desc[];
extern const char    *portal_string_desc[];
extern const int      portal_string_desc_count;
