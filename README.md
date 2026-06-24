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
| Sync model | **Transparent tunnel** — every HID report is forwarded both ways, so figures, Trap Team **traps**, the portal **speaker/LED** effects and multiple figures all pass through (see below). |

## How the tunnel works

The console polls the portal and streams status, so timing is tight. The bridge
forwards **every HID report** verbatim in both directions — that pass-through is
what makes Trap Team **traps**, the portal **speaker/LED effects** and **multiple
figures** work. To stay inside the console's timing budget the T-Dongle:

- answers `Ready`/`Activate` **locally** for instant detection, so the console's
  first portal check never waits for a radio round-trip;
- keeps the **audio stream off the command path** — data on the portal's
  interrupt OUT endpoint is forwarded to the *real* portal's OUT endpoint, never
  mis-parsed as a command (parsing audio bytes as commands would otherwise toggle
  the portal off and drop the figure);
- **ACKs and retries writes** over the link, on top of the pre-write backup below.

## Figure-corruption protection (writes)

We write the **real** figure but guard against leaving it in a bad state:

1. **Pre-write backup** — before the first write to a given tag, the N16R8 saves
   that figure's full image to NVS (keyed by its UID). Hold the **N16R8's BOOT
   button** to restore the figure currently on the portal.
2. **ACK + retry** — each write is acknowledged over the link and retried on
   packet loss, so a dropped radio frame can't silently skip a block.
3. **Atomic by design** — Skylanders store save data in two areas with a sequence
   number + checksum; the game writes the *inactive* area and flips the sequence
   in a final block. A faithfully-relayed write is therefore atomic — an
   interrupted write leaves the previous area valid. We relay it, we don't
   reimplement it.

> Note: because the **real portal** does the Mifare authentication and NFC I/O,
> the ESP firmware never needs the figure's encryption keys — it only sends
> Query/Write commands and relays bytes. That keeps the firmware simpler and the
> crypto out of scope.

## Auto-reconnect & display

ESP-NOW peers are symmetric: each board stores the peer MAC (paired once), and
sends a periodic `HELLO`/`HEALTH` heartbeat. "Searching" = broadcasting discovery
beacons until a peer answers; "linked" = heartbeats flowing. The T-Dongle screen
shows a title bar plus `LINK OK −xx dB`, `USB CONNECTED`, `FIG <toy-id> x<count>`,
a write/audio packet counter, and a short event log.

## Repo layout

```
skylanders_tracker/
├── README.md
├── shared/
│   ├── sky_protocol.h   ← Portal-of-Power USB protocol + CRC16 + helpers
│   └── link_protocol.h  ← ESP-NOW message format (both boards)
├── portal-side/         ← N16R8 firmware        (USB host, transparent + write-safety)
└── console-side/        ← T-Dongle-S3 firmware  (USB device, + ST7735 status display)
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
   pio run -e console-side -d console-side -t upload --upload-port /dev/ttyACM0
   pio run -e portal-side  -d portal-side  -t upload --upload-port /dev/ttyACM0
   ```
3. Tap **RESET** (or replug) to leave download mode and run the firmware.

> For flashing, use each board's **native** USB port in download mode (the
> N16R8's USB-C #1, the T-Dongle's USB-A — see *Wiring* above for the full
> pinout). To restore a figure's pre-write backup, hold the **N16R8's BOOT
> button**.

### Patched dependencies (applied automatically)
Two Espressif managed components need small local patches. You **don't apply them
by hand**: [`shared/patch_components.py`](shared/patch_components.py) runs from
each project's `CMakeLists.txt` right after the component manager fetches them
(idempotent), so a clean build just works. `managed_components/` itself is
git-ignored and regenerated on build.

- **`espressif__tinyusb` › `src/class/hid/hid_device.c`** — tag interrupt-OUT
  reports with `report_id 0xEE` so the app can keep the portal **audio** stream
  off the command path.
- **`espressif__usb_host_hid` › `hid_host.c` (+ `include/usb/hid_host.h`)** — add
  `hid_host_device_output_report()` to drive the portal's interrupt **OUT**
  endpoint (audio / trap-LED); the stock driver only exposes control `SET_REPORT`.
