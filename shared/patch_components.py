#!/usr/bin/env python3
"""
Re-apply our local patches to the fetched ESP-IDF managed components.

The IDF component manager fetches `managed_components/` clean on every (re)config
and would wipe hand edits, so instead of committing patched components we keep
them out of git and re-apply the patches here. This runs from each project's
top-level CMakeLists.txt (via execute_process, right after project()), i.e. after
the components are fetched but before ninja compiles them. It is idempotent.

Usage: patch_components.py <project_dir>
"""
import os
import sys

# Each patch: (relative path under managed_components, marker that means
# "already patched", text to find, replacement text).
PATCHES = [
    # --- usb_host_hid: track an interrupt OUT transfer on the interface ---
    (
        "espressif__usb_host_hid/hid_host.c",
        "out_xfer;",
        "    usb_transfer_t *in_xfer;                /**< Pointer to IN transfer buffer */\n"
        "    hid_host_interface_event_cb_t user_cb;  /**< Interface application callback */",
        "    usb_transfer_t *in_xfer;                /**< Pointer to IN transfer buffer */\n"
        "    usb_transfer_t *out_xfer;               /**< PATCH: interrupt OUT transfer (portal audio/LED) */\n"
        "    volatile bool   out_busy;               /**< PATCH: out_xfer currently in flight */\n"
        "    hid_host_interface_event_cb_t user_cb;  /**< Interface application callback */",
    ),
    # --- usb_host_hid: add a public interrupt-OUT sender ---
    (
        "espressif__usb_host_hid/hid_host.c",
        "hid_host_device_output_report",
        "/** Lock HID device from other task",
        "/**\n"
        " * @brief PATCH: interrupt OUT transfer complete — just clear the busy flag.\n"
        " */\n"
        "static void out_xfer_done(usb_transfer_t *out_xfer)\n"
        "{\n"
        "    hid_iface_t *iface = (hid_iface_t *) out_xfer->context;\n"
        "    if (iface) iface->out_busy = false;\n"
        "}\n"
        "\n"
        "/**\n"
        " * @brief PATCH: send a raw report to the device's interrupt OUT endpoint.\n"
        " */\n"
        "esp_err_t hid_host_device_output_report(hid_host_device_handle_t hid_dev_handle,\n"
        "                                        uint8_t ep_addr,\n"
        "                                        const uint8_t *data, size_t len)\n"
        "{\n"
        "    hid_iface_t *iface = get_iface_by_handle(hid_dev_handle);\n"
        "    HID_RETURN_ON_INVALID_ARG(iface);\n"
        "    HID_RETURN_ON_INVALID_ARG(iface->parent);\n"
        "    if (len == 0 || len > 64) return ESP_ERR_INVALID_SIZE;\n"
        "\n"
        "    if (iface->out_xfer == NULL) {\n"
        "        HID_RETURN_ON_ERROR( usb_host_transfer_alloc(64, 0, &iface->out_xfer),\n"
        "                             \"Unable to allocate OUT transfer\");\n"
        "        iface->out_busy = false;\n"
        "    }\n"
        "    if (iface->out_busy) return ESP_ERR_INVALID_STATE;   /* still sending -> drop */\n"
        "\n"
        "    usb_transfer_t *x = iface->out_xfer;\n"
        "    memcpy(x->data_buffer, data, len);\n"
        "    x->device_handle    = iface->parent->dev_hdl;\n"
        "    x->bEndpointAddress = ep_addr;                       /* 0x02, OUT */\n"
        "    x->callback         = out_xfer_done;\n"
        "    x->context          = iface;\n"
        "    x->num_bytes        = len;\n"
        "    x->timeout_ms       = 100;\n"
        "    iface->out_busy = true;\n"
        "    esp_err_t err = usb_host_transfer_submit(x);\n"
        "    if (err != ESP_OK) iface->out_busy = false;\n"
        "    return err;\n"
        "}\n"
        "\n"
        "/** Lock HID device from other task",
    ),
    # --- usb_host_hid: declare the sender in the public header ---
    (
        "espressif__usb_host_hid/include/usb/hid_host.h",
        "hid_host_device_output_report",
        "esp_err_t hid_class_request_set_report(hid_host_device_handle_t hid_dev_handle,\n"
        "                                       uint8_t report_type,\n"
        "                                       uint8_t report_id,\n"
        "                                       uint8_t *report,\n"
        "                                       size_t report_length);",
        "esp_err_t hid_class_request_set_report(hid_host_device_handle_t hid_dev_handle,\n"
        "                                       uint8_t report_type,\n"
        "                                       uint8_t report_id,\n"
        "                                       uint8_t *report,\n"
        "                                       size_t report_length);\n"
        "\n"
        "/* PATCH: send a raw report to the device's interrupt OUT endpoint (e.g. 0x02). */\n"
        "esp_err_t hid_host_device_output_report(hid_host_device_handle_t hid_dev_handle,\n"
        "                                        uint8_t ep_addr,\n"
        "                                        const uint8_t *data, size_t len);",
    ),
    # --- tinyusb: tag interrupt-OUT-endpoint reports so the app can tell them
    #     apart from control SET_REPORT commands (Skylanders audio path). ---
    (
        "espressif__tinyusb/src/class/hid/hid_device.c",
        "report_id 0xEE",
        "      tud_hid_set_report_cb(instance, 0, HID_REPORT_TYPE_OUTPUT, p_epbuf->epout, (uint16_t)xferred_bytes);",
        "      /* PATCH (wireless portal): report_id 0xEE marks data from the\n"
        "         interrupt OUT endpoint (Skylanders audio) vs control commands. */\n"
        "      tud_hid_set_report_cb(instance, 0xEE, HID_REPORT_TYPE_OUTPUT, p_epbuf->epout, (uint16_t)xferred_bytes);",
    ),
]


def main():
    if len(sys.argv) < 2:
        print("usage: patch_components.py <project_dir>", file=sys.stderr)
        return 0  # don't fail the build
    base = os.path.join(sys.argv[1], "managed_components")
    for rel, marker, old, new in PATCHES:
        path = os.path.join(base, rel)
        if not os.path.isfile(path):
            continue                      # component not present in this project
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
        if marker in text:
            continue                      # already patched
        if old not in text:
            print(f"[patch_components] WARNING: anchor not found in {rel} "
                  f"(component version changed?)", file=sys.stderr)
            continue
        with open(path, "w", encoding="utf-8") as f:
            f.write(text.replace(old, new, 1))
        print(f"[patch_components] patched {rel}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
