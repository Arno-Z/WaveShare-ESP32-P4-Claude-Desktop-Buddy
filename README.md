# Claude Face — a desk buddy for Claude Desktop on the Waveshare ESP32-P4 3.4C

A port of Anthropic's [Claude Desktop Hardware Buddy][buddy] BLE companion
protocol to the [Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C][board] — a dual-core
RISC-V board with a 3.4" 800×800 round MIPI-DSI touchscreen, an ESP32-C6
co-processor for BLE, and a sweetly-tuned display pipeline.

The device sits on your desk, pairs with Claude Desktop over Bluetooth LE, and
*glows*. It shows what Claude is up to, asks you to tap a touchscreen button
when Claude needs permission to run a tool, and streams that decision back to
the desktop. All over AES-CCM-encrypted BLE with LE Secure Connections bonding.

[buddy]: https://github.com/anthropics/claude-desktop-buddy
[board]: https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-3.4c.htm

## What works today

- **Pairing**: LE Secure Connections with DisplayOnly IO capability. First
  connect triggers a macOS Bluetooth pairing dialog; the 6-digit passkey shows
  on the aperture. Bond persists in NVS; silent re-encrypt on reconnect.
  Multiple macs supported (up to 8 bonds).
- **State mirroring**: `total/running/waiting` snapshots drive a 7-state face
  — SLEEP, IDLE, BUSY, ATTENTION, APPROVED, DENIED, CELEBRATE — each with its
  own colour + breathing animation. Transitions cross-fade between states;
  APPROVED blooms past full size then settles.
- **Permission prompts**: when Claude asks for tool approval the face flips
  amber, shows the tool + command hint, and presents **Approve** / **Deny**
  buttons on the touchscreen. Decision streams back as
  `{"cmd":"permission","id":"…","decision":"once"|"deny"}`.
- **Audio feedback**: ES8311 + speaker. Soft two-tone chime on ATTENTION
  (keyed off a new prompt id, not every heartbeat); bright pluck on Approve;
  low muted thud on Deny.
- **Folder push**: the Hardware Buddy window's drop target works — dropped
  folders stream into `/spiffs/assets/<name>/` via the
  `char_begin`/`file`/`chunk`/`file_end`/`char_end` protocol, with path
  sanitation.
- **Commands implemented**: `status` (with live uptime, heap, encryption flag),
  `owner`, `name`, `unpair` (erases all stored bonds).
- **Testing harness**: `tools/fake_buddy` — a Python BLE central that
  impersonates Claude Desktop so you can drive the face from a keyboard.

## What's still ahead

- Do something useful with pushed assets — wire SPIFFS content into the face
  (custom character images, mascot sprites, WAV chimes loaded from dropped
  folders).
- Battery / charging telemetry in the `status` ack (currently reports name,
  sec, up, heap only).
- CELEBRATE state is drawn but never fires — no upstream signal for it yet.
- Chase the harmless-but-ugly `ledc: GPIO 26 is not usable` warning at boot
  (LCD backlight PWM channel collision with the BSP display init order).

## Hardware

- Waveshare **ESP32-P4-WIFI6-Touch-LCD-3.4C** (the 3.4" variant — 115 mm
  outline, not the 4". If yours is 126 mm outside it's the 4C and the
  display timings will differ).
- JD9365 MIPI-DSI LCD controller, GT911 capacitive touch.
- ESP32-C6-MINI-1 co-processor over SDIO (stock Waveshare firmware works
  — no re-flash of the C6 needed).
- 32 MB GigaDevice QIO NOR flash, 32 MB hex-PSRAM @ 200 MHz.

## Quick start

See [INSTALL.md](./INSTALL.md) for a step-by-step setup. The short version:

```sh
# 1. ESP-IDF 5.5 installed (esp-idf v5.5.4 used here)
. $HOME/esp/esp-idf/export.sh

# 2. Clone, configure, build, flash
git clone https://github.com/Arno-Z/WaveShare-ESP32-P4-Claude-Desktop-Buddy.git
cd WaveShare-ESP32-P4-Claude-Desktop-Buddy
./idf build
./idf -p /dev/cu.usbmodemXXXXXXX flash

# 3. Pair with Claude Desktop
#   Help → Troubleshooting → Enable Developer Mode
#   Developer → Open Hardware Buddy → Connect → "Claude Face"
#   enter the passkey the device displays
```

The `./idf` wrapper sources `esp-idf/export.sh` before every invocation so you
can run `./idf build`, `./idf flash`, `./idf monitor` without manually
sourcing.

## Repository layout

```
main/
  main.c             -- app_main: BSP display init, face build, BLE start
  face.c / face.h    -- LVGL aperture + message + decision buttons + passkey panel
  ble_nus.c / .h     -- Nordic UART Service GATT, LE-SC pairing, bond store
  cdb_protocol.c /.h -- newline-JSON framer + dispatch + status ack
  audio.c / .h       -- I2S + ES8311 chime on ATTENTION
  idf_component.yml  -- depends on waveshare BSP + esp_hosted + esp_wifi_remote
tools/
  fake_buddy         -- local BLE central for driving the face without Claude Desktop
  fake_buddy.py      -- the script
case/
  WaveShare-untested.step
                     -- first-pass enclosure STEP, NOT YET printed/tested
  README.md          -- print notes + caveats
DESIGN.md            -- face design doc (7-state visual language)
claude-desktop-buddy-port.md
                     -- original porting plan + protocol summary
sdkconfig.defaults   -- load-bearing config (pre-v3 P4 landmines documented inline)
partitions.csv       -- custom 4 MB factory app partition
CMakeLists.txt       -- disables -msmall-data-limit (pre-v3 P4 .sdata quirk)
```

## Enclosure

A first-pass STEP file lives under [`case/`](./case/). It has not
been printed or test-fitted to real hardware yet — treat it as a
starting point, not a finished design. See
[`case/README.md`](./case/README.md) for print notes and how to
report what fits / what doesn't.

## Why "Claude Face"

Because the round 3.4" display looked, from the moment we first drove a pixel,
like it was staring back. The aperture is the eye; the rest follows from
there. See [DESIGN.md](./DESIGN.md) for the visual language.

## Status

Phase 3 complete (secure bonding, live Claude Desktop interaction, touch
approve/deny round-trip). Not an officially-supported Anthropic product; this
is an independent port built around their open protocol spec.
