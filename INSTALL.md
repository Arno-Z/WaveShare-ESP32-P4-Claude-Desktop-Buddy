# INSTALL — setting up Claude Face from scratch

This guide is written to be executable literally by [Claude Code] with
minimal user intervention. A human can follow it just as easily; each
step has a success check you can verify before moving on.

[Claude Code]: https://claude.com/claude-code

## Target audience and assumptions

- **OS**: macOS 12+ (Intel or Apple Silicon). Linux and Windows work for
  the build/flash loop but the BLE pairing UI here is macOS-specific.
- **Hardware in hand**:
  - Waveshare **ESP32-P4-WIFI6-Touch-LCD-3.4C** (the 3.4" variant).
    The 4C (126 mm outline) uses different LCD timings and is not
    supported by this firmware without changes.
  - USB-C cable (data-capable, not charge-only).
- **Desktop app**: Claude Desktop for macOS installed from
  <https://claude.ai/download>. The BLE bridge does not work with the
  web app or with Claude Code in a terminal.

## Pre-flight check

Before running anything else, confirm the following. Claude Code can
execute each probe and parse the output to decide whether to proceed.

```sh
# Must be macOS. Script below also works on Linux for build only.
uname -s                                      # → Darwin (or Linux)

# Must have Homebrew (macOS) or equivalent. Auto-install if missing.
command -v brew                               # → /opt/homebrew/bin/brew

# A working Python 3.10+ for the tester.
python3 --version                             # → Python 3.10.x+

# Free disk: IDF toolchain is ~2.5 GB.
df -g "$HOME" | tail -1 | awk '{print $4}'    # need ≥ 5 (GB free)
```

## 1. Install ESP-IDF 5.5

The firmware pins `idf: ">=5.5"` in `main/idf_component.yml`. Version
5.5.4 is the stable target. Install via Espressif's official installer:

```sh
# Dependencies (macOS).
brew install cmake ninja dfu-util ccache

# Fetch IDF + toolchain into the canonical location.
mkdir -p "$HOME/esp"
cd "$HOME/esp"
git clone --branch v5.5.4 --depth 1 --recurse-submodules \
    https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32p4
```

Success check:

```sh
. "$HOME/esp/esp-idf/export.sh"
idf.py --version                  # → ESP-IDF v5.5.x
```

## 2. Clone the project

```sh
cd ~/espprojects 2>/dev/null || { mkdir -p ~/espprojects && cd ~/espprojects; }
git clone https://github.com/Arno-Z/WaveShare-ESP32-P4-Claude-Desktop-Buddy.git
cd WaveShare-ESP32-P4-Claude-Desktop-Buddy
```

## 3. Find the USB port

Plug the board in with USB-C. It enumerates as a CH343-based virtual
serial port (Waveshare ships a WCH USB-UART bridge on the board).

```sh
ls /dev/cu.usbmodem* 2>/dev/null
# → /dev/cu.usbmodemXXXXXXXX   (id varies between boards and sessions)
```

If nothing shows up:

- try a different cable (charge-only cables do not work),
- try the other USB-C port on the board if it has two (one is the
  power-only port),
- make sure the board power LED is lit.

Export the port for convenience:

```sh
export PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
[ -n "$PORT" ] && echo "using $PORT"
```

## 4. First build + flash

The repo ships a thin wrapper `./idf` that sources `$HOME/esp/esp-idf/export.sh`
before each invocation — you do not need to manually source anything.

```sh
./idf set-target esp32p4        # writes sdkconfig from sdkconfig.defaults
./idf build                     # ~3-5 min first time (managed components fetch)
./idf -p "$PORT" flash          # writes bootloader + partition + app
```

Success check (serial log, Ctrl-] to exit):

```sh
./idf -p "$PORT" monitor
```

You should see the board boot, the Waveshare BSP init the LCD + touch,
then `ble-nus: advertising as "Claude Face"`. On the display: a dim
halo + a closed horizontal slit (SLEEP state) and the text "not
connected" below.

## 5. Pair with Claude Desktop

1. **Enable Developer Mode** in Claude Desktop: macOS menu bar →
   **Help → Troubleshooting → Enable Developer Mode**.
2. A new **Developer** menu appears. Click **Developer → Open
   Hardware Buddy…**
3. In the pairing window, click **Connect** and pick **Claude Face**.
4. The board displays a 6-digit passkey (big orange digits on black
   with "Pairing" and "enter this code on your Mac" captions).
5. macOS pops a Bluetooth Pairing Request dialog — type the 6 digits.
6. Pairing completes, passkey screen disappears, face goes to IDLE
   (breathing orange circle) with a brief "connected" label.

From here, Claude Desktop's chat activity drives the face. Tool-call
permission prompts trigger ATTENTION with Approve/Deny buttons.

## 6. Verify without Claude Desktop (optional)

Claude Code can test end-to-end without the Desktop app using the
bundled `fake_buddy` tool. First disconnect Claude Desktop's Hardware
Buddy window (the board only accepts one central at a time).

```sh
./tools/fake_buddy              # creates a local venv + bleak, connects
```

Keys inside the tool:

- `t` → send `{"cmd":"status"}`; you should see `"sec":true` in the
  ack, confirming AES-CCM encryption.
- `a` → send an ATTENTION heartbeat with a pretend Bash permission
  request; face flips amber with the tool hint and shows approve/deny
  buttons.
- `b` / `i` / `s` → BUSY / IDLE / SLEEP snapshots.
- `u` → send `{"cmd":"unpair"}`; the board erases all stored bonds
  and disconnects. Next pair will need a fresh passkey exchange.
- `q` → quit.

## Troubleshooting

### "CONFIG_ESP32P4_SELECTS_REV_LESS_V3" landmines

Pre-v3 ESP32-P4 silicon (revision v1.0 or older) has an IDF-5.5 linker
regression where `.sdata` sections silently overflow the GP-addressable
window and the `--enable-non-contiguous-regions` flag discards them.
The `sdkconfig.defaults` in this repo carries workarounds for all of
them — **don't delete comments in that file**, they explain why each
knob is required. Notably:

- `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n` is the single biggest win
  (frees ~112 KB of IRAM by keeping LVGL in flash/XIP PSRAM).
- `CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH=y` frees ~3 KB.
- `CONFIG_CACHE_L2_CACHE_128KB=y` enlarges `sram_high` to 384 KB.

If a future BSP update undoes any of these, the link will fail with
"discards section" errors pointing at spi_flash / newlib / heap code.

### Passkey dialog doesn't appear

- Make sure you haven't previously bonded this device on this Mac with
  a bad key. System Settings → Bluetooth → Claude Face → ⓘ → **Forget
  This Device**, then re-try from step 5.3.
- If pairing via `fake_buddy` (bleak) instead of Claude Desktop, macOS
  sometimes suppresses the pairing dialog for non-GUI processes. Pair
  once via Claude Desktop — the bond then works for `fake_buddy`
  re-connects because macOS shares the bond across apps.

### Stock C6 firmware RPC timeouts in logs

Lines like
`Version mismatch: Host [2.12.0] > Co-proc [0.0.0] ==> Upgrade co-proc`
and `rpc_core: Timeout waiting for Resp for Req_GetCoprocessorFwVersion`
are **expected and harmless**. The Waveshare stock C6 firmware speaks
an older esp-hosted RPC, but the VHCI transport for BLE works anyway.
This codebase skips the RPC gate calls — see
`main/ble_nus.c` comments above `esp_hosted_connect_to_slave()`.

### "device not configured" during serial capture

The USB-serial-JTAG bridge can drop briefly during chip reset. Re-open
the port; the firmware is unaffected.
