# Wireless Skylanders Portal (ESP-NOW bridge)

Make a Skylanders Portal of Power **wireless**: place figures on a real portal in
one place, play on a console (Wii / Wii U / PS3 / PS4) somewhere else. Two
ESP32-S3 boards bridge the portal's USB over a 2.4 GHz ESP-NOW link.

> ⚠️ **Console support:** Wii, Wii U, PS3 and PS4 — these all use the same wired
> Portal of Power USB-HID protocol. **Xbox is NOT supported** — the Xbox 360 / One
> Skylanders portals use a different, proprietary RF/USB scheme, so this emulation
> will not work with them.

```
[Figure] ─NFC→ [Real Portal of Power] ──USB──▶ [ESP32-S3-N16R8]
                                         (USB HOST)     │
                                                        │  ESP-NOW (2.4 GHz)
                          figure image + presence  ───▶ │  ◀─── console writes
                                                        ▼
[Console] ◀─USB─ [ T-Dongle-S3 ] ◀─────────────────────┘
                 (USB DEVICE = emulated portal;
                  ST7735 shows link + figure status)
```

## Design decisions

| Topic | Decision |
|-------|----------|
| Portal-side input | **Real Portal of Power via USB host** (N16R8 OTG port) |
| Console-side | **T-Dongle-S3 emulates the portal** (TinyUSB device) |
| Wireless link | **ESP-NOW** (connectionless, ~1–2 ms, symmetric peers, robust reconnect) |
| Write handling | **Write the real figure, with a safety net** (see below) |
| Sync model | **Two variants** — v1 state-replication (cached image) or v2 transparent tunnel (full pass-through, incl. traps & sound). See [Two firmware variants](#two-firmware-variants). |

## Sync model: replication (v1) vs tunnel (v2)

The console polls the portal and streams status; if every USB transaction had to
round-trip over the radio, the console could time the portal out.

**v1 (state replication)** sidesteps this: the T-Dongle keeps a **local cached
copy** of each present figure (its 1 KB image) and answers Activate/Status/Query
instantly. Only three things cross the link:

1. **presence** (figure placed/removed + toy-ID) — tiny, frequent,
2. **figure image** (1 KB, chunked) — once per placement,
3. **write-back** (console → figure save data) — handled with the safety net.

**v2 (transparent tunnel)** forwards every HID report instead, so traps, the
portal speaker/LED effects and multiple figures all pass through. To stay within
the console's timing it answers `Ready`/`Activate` locally for instant detection,
keeps the audio stream off the command path (routing it to the portal's OUT
endpoint), and ACKs + retries writes over the link.

## Figure-corruption protection (writes)

We write the **real** figure but never leave it in a bad state:

1. **Pre-write backup** — before the first write to a figure, the N16R8 dumps its
   full 1 KB to TF card. One-click restore if anything ever looks wrong.
2. **Buffer, then write locally** — the T-Dongle collects the console's complete
   write set and the N16R8 **ACKs it before touching NFC**, so a dropped radio
   link can never interrupt a write mid-sequence (the radio is out of the loop
   during the actual NFC write).
3. **Lean on the game's native dual-save-area scheme** — Skylanders store save
   data in two areas with a sequence number + checksum; the game writes the
   *inactive* area and flips the sequence in a final block. A faithfully-relayed
   write is therefore atomic by design: an interrupted write leaves the previous
   area valid. We do **not** reimplement this — we relay it faithfully.
4. **Read-back-verify** — after writing, the N16R8 re-reads the affected blocks
   and compares byte-for-byte (optionally CRC16); mismatch → retry, never commit.
5. **Brown-out guard** — refuse to *start* an NFC write if VBUS/Vcc is marginal;
   a bulk capacitor / supercap lets an in-flight block finish on power loss.

> Note: because the **real portal** does the Mifare authentication and NFC I/O,
> the ESP firmware never needs the figure's encryption keys — it only sends
> Query/Write commands and relays bytes. That keeps the firmware simpler and the
> crypto out of scope.

## Auto-reconnect & display

ESP-NOW peers are symmetric: each board stores the peer MAC (paired once), and
sends a periodic `HELLO`/`HEALTH` heartbeat. "Searching" = broadcasting discovery
beacons until a peer answers; "linked" = heartbeats flowing. The T-Dongle screen
shows: `SCANNING…` → `LINKED  rssi −xx` → `Figur: Spyro` → `WRITE…` etc.

## Two firmware variants

This repo ships **two complete implementations** — flash whichever fits:

- **v1 — state replication** (`console-side/`, `portal-side/`): the dongle answers
  the console locally from a cached figure image; only presence/image/writes cross
  the radio. Lowest latency, but no live sound / trap-light pass-through.
- **v2 — transparent tunnel** (`v2-tunnel/`): every HID report is forwarded both
  ways, so multi-figure, Trap Team **traps**, and the portal **speaker/LED**
  effects work. The dongle answers `Ready`/`Activate` locally for instant console
  detection, keeps audio off the command path, and routes it to the portal's OUT
  endpoint. **Recommended.**

## Repo layout

```
skylanders_tracker/                ← this repo (the ESP side)
├── README.md
├── shared/
│   ├── sky_protocol.h   ← Portal-of-Power USB protocol + CRC16 + helpers
│   └── link_protocol.h  ← ESP-NOW message format (both boards)
├── portal-side/         ← v1  N16R8        (USB host)
├── console-side/        ← v1  T-Dongle-S3  (USB device)
└── v2-tunnel/
    ├── portal-side/     ← v2  N16R8        (USB host, transparent + write-safety)
    └── console-side/    ← v2  T-Dongle-S3  (USB device, + ST7735 status display)
```

The `shared/` headers are framework-agnostic C and are the contract both
firmwares implement.

## Installation & Build

### Prerequisites
- **PlatformIO Core** (CLI). For example in a venv:
  ```bash
  python3 -m venv ~/.pio-venv
  ~/.pio-venv/bin/pip install platformio
  ```
  The ESP-IDF toolchain and the `espressif32` platform are fetched automatically
  on the first build.
- **Hardware:**
  - **LilyGo T-Dongle-S3 — console side.**
    [AliExpress](https://de.aliexpress.com/item/1005004860003638.html)
    ⚠️ Pick the **"T-Dongle-S3 with LCD"** variant — the 0.96" ST7735 screen is
    what shows the link/figure status.
  - **ESP32-S3-N16R8 — portal side.**
    [AliExpress](https://de.aliexpress.com/item/1005008752956562.html)
    ⚠️ In the option dropdown select **"ESP32-S3-N16R8"** (16 MB Flash / 8 MB
    PSRAM), the **dual USB-C** board — one port is the USB-OTG **host** to the
    portal. (Not the N8/N8R8/single-port variants.)
  - A real **Portal of Power** (Trap Team / Traptanium recommended for full
    compatibility), USB cables, and a **5 V feed for the portal's VBUS** (see
    *Wiring* below — the #1 gotcha).

### Build
Run from the repo root (`source ~/.pio-venv/bin/activate` first, or prefix the
commands with `~/.pio-venv/bin/`):

```bash
# v2 (recommended)
pio run -e portal-side-v2  -d v2-tunnel/portal-side
pio run -e console-side-v2 -d v2-tunnel/console-side

# v1
pio run -e portal-side  -d portal-side
pio run -e console-side -d console-side
```

### Wiring & the USB-OTG solder jumper

> ⚠️ **You must bridge the N16R8's `USB-OTG` solder jumper with a blob of solder.**
> The ESP32-S3-N16R8's native USB-C port ships wired as a USB *device*. The small
> **`USB-OTG`** solder pad next to that connector must be **closed** to enable
> host mode — otherwise the board cannot act as a USB host and the portal is
> never detected.

```
  [ Figure ]
      | NFC
      v
  +--------------------------------------+
  | Real Portal of Power                 |
  +------------------+-------------------+
                     | USB data + 5 V VBUS   <-- external 5 V supply (GND common)
                     v
  +--------------------------------------+
  | ESP32-S3-N16R8        (PORTAL SIDE)  |
  |  - USB-C #1 (native) = USB HOST      |
  |      /!\ OTG solder jumper BRIDGED   |
  |  - USB-C #2 (CH343)  = power / logs  |
  |      / serial / flashing             |
  +------------------+-------------------+
                     | ESP-NOW   (2.4 GHz, wireless)
                     v
  +--------------------------------------+
  | T-Dongle-S3          (CONSOLE SIDE)  |
  |  - USB-A = USB DEVICE (emul. portal) |
  |  - ST7735 LCD = link/figure status   |
  +------------------+-------------------+
                     | USB
                     v
  +--------------------------------------+
  | Console -- Wii / Wii U / PS3 / PS4   |
  +--------------------------------------+
```

**Powering the portal:** the native OTG port does **not** source 5 V on its own.
Feed **5 V to the portal's VBUS** from an external 5 V supply (or the N16R8's
USB-C #2 rail), with **GND common** to the N16R8. The portal can draw ~500 mA
with LEDs/RF, so don't undersize the supply.

### Flash — note: download mode is required
Both boards run firmware that **takes over the native USB**, so you can't flash
over it while it runs. Put the board into **download/boot mode** first:

1. Hold **BOOT**, tap **RESET** (or hold BOOT while plugging the **native** USB
   port in). The board then enumerates as a USB-JTAG device — `303a:1001`,
   typically `/dev/ttyACM0`.
2. Flash to that port:
   ```bash
   pio run -e console-side-v2 -d v2-tunnel/console-side -t upload --upload-port /dev/ttyACM0
   pio run -e portal-side-v2  -d v2-tunnel/portal-side  -t upload --upload-port /dev/ttyACM0
   ```
3. Tap **RESET** (or replug) to leave download mode and run the firmware.

> **Wiring:** N16R8 native USB-C = USB **host** (to the portal; must supply 5 V
> VBUS); its second USB-C (CH343) = power + serial logs. T-Dongle USB-A = USB
> **device** (to the console). For flashing, use each board's **native** USB port
> in boot mode. On the T-Dongle, BOOT-button restore re-flashes a figure's
> pre-write backup (v2).

### Patched dependencies (already committed)
`managed_components/` is committed **with local patches**. If the IDF component
manager ever re-fetches them clean, re-apply:

- **`v2-tunnel/console-side/managed_components/espressif__tinyusb/src/class/hid/hid_device.c`**
  — tag interrupt-OUT reports with `report_id 0xEE` so the app can keep the portal
  **audio** stream off the command path.
- **`v2-tunnel/portal-side/managed_components/espressif__usb_host_hid/hid_host.c`**
  (+ `include/usb/hid_host.h`) — add `hid_host_device_output_report()` to drive the
  portal's interrupt **OUT** endpoint (audio / trap-LED); the stock driver only
  exposes control `SET_REPORT`.
- **`portal-side/managed_components/espressif__led_strip`** (v1 only) — add
  `#include "esp_heap_caps.h"` for IDF 6.0 compatibility.
