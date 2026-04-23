# Orienting note for Claude Code agents

If you've been asked to help develop, debug, extend, or install this
project, start here.

## What this is

Firmware for a desk "face" device that mirrors Claude Desktop's state
over BLE and lets you approve/deny tool-call permissions from a
touchscreen. See [README.md](./README.md) for the feature tour and
[INSTALL.md](./INSTALL.md) for a step-by-step build + pair guide.

## Hardware that *must* match

- **Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C** (3.4", 115 mm outline,
  800×800 round JD9365 LCD, GT911 touch, ESP32-C6-MINI-1 radio
  co-processor). The 4C variant (126 mm) is NOT supported without
  LCD timing changes.
- ESP32-P4 v1.0 silicon is what we develop against. The codebase
  carries workarounds for the IDF-5.5 pre-v3 linker quirks; newer
  v3+ silicon should also work but isn't exercised.

## Toolchain

- ESP-IDF 5.5.4 at `$HOME/esp/esp-idf`.
- The `./idf` wrapper at repo root sources `export.sh` for you — use
  `./idf build`, `./idf -p /dev/cu.usbmodemXXX flash`, etc.
- `tools/fake_buddy` is a Python BLE central for local testing. It
  maintains its own `.venv` under `tools/.venv` (gitignored).

## Load-bearing configuration

`sdkconfig.defaults` contains inline comments explaining each
non-obvious knob. **Do not remove those comments.** In particular,
the following are load-bearing on pre-v3 P4 + IDF 5.5:

- `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n` — frees ~112 KB of IRAM.
  Without this, the link fails with cryptic `--enable-non-contiguous-regions
  discards section` errors from spi_flash / newlib.
- `CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH=y`.
- `CONFIG_CACHE_L2_CACHE_128KB=y` / `CONFIG_CACHE_L2_CACHE_LINE_128B=y`.
- `CONFIG_LIBC_MISC_IN_IRAM=n`, `CONFIG_LIBC_LOCKS_PLACE_IN_IRAM=n`.
- `CMakeLists.txt` adds `-msmall-data-limit=0` after `project()` to
  disable small-data optimisation — another pre-v3 P4 workaround.

## Protocol sources of truth

- Upstream protocol spec: <https://github.com/anthropics/claude-desktop-buddy>
  (file `REFERENCE.md`). Newline-delimited JSON over Nordic UART Service,
  UUIDs `6e400001/2/3-b5a3-f393-e0a9-e50e24dcca9e`.
- **Only `"once"` and `"deny"` are valid `decision` values** — we
  tested adding `"always"` and Claude Desktop rejects it.
- The stock Waveshare C6 firmware reports as version 0.0.0 and does
  not respond to the esp-hosted 2.12 RPC gate calls
  (`esp_hosted_bt_controller_init`/`_enable`). These are explicitly
  skipped in `ble_nus.c`; the VHCI transport comes up via
  `esp_hosted_connect_to_slave()` alone and NimBLE's HCI RESET does
  the rest. See the long comment above `esp_hosted_connect_to_slave()`.

## Coding conventions in this repo

- Avoid over-commenting. Only add comments when the "why" is
  non-obvious (a hidden constraint, a vendor quirk, a workaround).
- When adding dev tools (new subcommands, analyzers, flashers),
  update `.claude/settings.local.json` so they run without
  permission prompts. Never grant blanket destructive access.
- Keep `sdkconfig.defaults` canonical; the `sdkconfig` file is
  gitignored and regenerates from defaults on a fresh `./idf
  reconfigure`.

## Testing workflow

1. `./idf build` — should succeed in ~20-40 s on an incremental build.
2. `./idf -p "$PORT" flash` — PORT = `/dev/cu.usbmodemXXX` on macOS.
3. `./tools/fake_buddy` for BLE-level smoke tests (see INSTALL.md
   §6). Don't leave Claude Desktop's Hardware Buddy window connected
   while running fake_buddy — only one central is allowed.

## What's not done yet

- Folder-push protocol (`char_begin` / `file` / `chunk` / `file_end` /
  `char_end`). We ack the chunk frames with `ok:false` and ignore
  `char_begin` so Claude Desktop times the drop out.
- Audio feedback via the on-board ES8311 codec + speaker.
- Richer animations (state-transition tweens, APPROVED sparkle).
- Battery / charging telemetry in the `status` ack.

## Repo layout at a glance

- `main/` — firmware C
- `tools/` — Python test harness + venv
- `DESIGN.md` — face visual language
- `claude-desktop-buddy-port.md` — the original porting plan with
  protocol summary and architectural notes
- `INSTALL.md` — step-by-step setup (written for Claude Code)
- `sdkconfig.defaults` + `partitions.csv` + `CMakeLists.txt` — build config
