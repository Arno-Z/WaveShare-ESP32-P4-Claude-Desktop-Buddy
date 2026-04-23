# Recommendations addendum — Claude Desktop Buddy port to Waveshare ESP32-P4-3.4C

Companion to `claude-desktop-buddy-port.md`. Phases in the original doc are
**unchanged** — Claude Code has just finished Phase 2 against that plan, and
the phase boundaries are a shipping contract at this point. These notes are
refinements for Phases 3–5 and cross-cutting concerns, plus retroactive
checks to run against the Phase 2 build before moving on.

---

## Retroactive checks on the Phase 2 build

Before Phase 3 starts touching SMP and flipping characteristics to
encrypted-only, run these on what Claude Code just shipped. Cheap, and
each one catches a class of bug that's painful to debug once bonding is
in the way.

### R1. MTU measurement (do this first)

Originally risk #2 in the main doc; elevating to a Phase 2 → 3 gate.

- Measure the negotiated MTU from macOS Claude Desktop against the current
  firmware. Log it in the status ack path or via `ESP_LOGI` on the
  `BLE_GAP_EVENT_MTU` event.
- Push a 1 MB dummy payload (synthetic `char_begin` / `file` / `chunk`
  sequence against a local test harness — see R4) and record wall-clock
  throughput end-to-end.
- Decision tree:
  - **≥ 180 B effective payload, ≥ 30 KB/s throughput** → proceed, Phase 4
    folder-push will be fine.
  - **MTU capped at 23 (20 B payload)** → stop. Either reflash the C6 with
    a newer `esp-hosted-mcu` network-adapter image, or descope Phase 4 to
    "character packs baked into the partition image, `char_begin` ack
    returns `ok:false`." Both are valid; pick before committing to Phase 4.
  - **Anything in between** → measure, then decide. Don't build Phase 4 on
    unvalidated throughput assumptions.

### R2. Permission default-state audit

The reference M5StickC requires a deliberate A-button press to approve.
Cap-touch on a desk-facing 3.4" panel does not have that physical-intent
property. Before Phase 3, verify:

- Timeout behavior on an unanswered `prompt`: does the current Phase 2 UI
  send a decision at all, and if so, which one?
- Any code path where an incoming GAP / GATT event can advance the UI
  state machine to the approval modal and have a "tap anywhere" handler
  still live.

Neither of these should default-approve. If they do, fix in Phase 2
before Phase 3 — once SMP is in the way, debugging gets harder.

### R3. GT9271 10-point sanity

The `esp_lcd_touch_gt911` driver sometimes hardcodes 5-point buffers,
and the panel here is 10-point. Not a blocker for single-tap UI, but
verify during LVGL indev readout that all 10 touches propagate. This
unlocks two-finger gestures as a potential deny input in Phase 3
(see S2 below).

### R4. Local protocol test harness

If Claude Code hasn't already produced one, this is a one-evening build
worth doing before Phase 3:

- Python BLE client using `bleak` that implements the desktop side of the
  wire protocol (advertise scanner → connect → MTU request → heartbeat
  sender → prompt sender → folder-push driver).
- Port the relevant assertions from upstream `test_serial.py` and
  `test_xfer.py`.
- Run it headless from the dev laptop, no Claude Desktop Developer Mode
  required.

This becomes the definition of "Phase 2 conformant" and makes Phase 3's
SMP work debuggable — you can exercise the protocol end-to-end without
needing to pair through the desktop every iteration.

---

## Phase 3 — Security sharpening

Original plan (LE SC + DisplayOnly + bonding + `unpair`) is correct. Two
additions:

### S1. Passkey display is a UX, not just a function call

NimBLE will post a passkey via `BLE_GAP_EVENT_PASSKEY_ACTION` with
`action = BLE_SM_IOACT_DISP`. Don't just `printf` it — the round panel
is the reason this device justifies DisplayOnly over JustWorks. Render:

- 6-digit passkey, large, monospace, centered in the safe-area rect.
- Outer arc as a countdown (NimBLE's SMP timeout is 30 s by default).
- Clear "Pairing with <initiator address>" subline; the peer address
  comes from the event struct.
- On success/failure: brief confirmation screen, then return to home.

### S2. Hold-to-confirm approval, default-deny

The approval modal design from §5 of the main doc (two half-circle targets)
is good, but cap-touch on a desk calls for stronger intent signaling:

- **Approve** = finger held on the top half-arc for 1.5 s, with a filling
  perimeter arc as visual feedback. Release early = cancel.
- **Deny** = single tap anywhere on the bottom half, OR any two-finger
  contact (uses R3's 10-point verification).
- **Timeout** = 20 s → send `{"decision":"deny"}`. Never default-approve.
- Render `prompt.tool` and `prompt.hint` prominently; both arrive in the
  heartbeat snapshot already.

This is stronger than the reference firmware's posture, which is
appropriate given the larger touch target and different threat model
(desk, not wrist).

### S3. `unpair` semantics

`ble_store_clear()` nukes every bond. If multiple desktops can bond to
one device (plausible — work Mac + personal Mac), consider whether
`unpair` should clear all bonds or prompt for which. Reference
implementation clears all; matching that is fine, but document the
decision in `REFERENCE.md`-equivalent notes for future-you.

---

## Phase 4 — Folder push

Gated on R1. Assuming throughput is adequate:

### F1. NVS schema lock-in (do this before Phase 4 writes production data)

`stats.h` in the reference firmware implies an NVS namespace. Lock the
schema now so Phase 4/5 don't force a migration:

```
namespace: "buddy"
  owner        : str    (from {"cmd":"owner"})
  species      : str    (current character pack name)
  level        : u32
  tokens_today : u32    (reset on midnight local, needs tz from time msg)
  tokens_total : u64
  turns_total  : u32
  bond_count   : u8     (for UI, not SMP state)
  last_reset   : u32    (epoch of last midnight reset)
```

Bond storage lives in NimBLE's own NVS namespace (`nimble_bond`), don't
collide.

### F2. Path validation — explicit test cases

`REFERENCE.md` says reject `..` and absolute paths. Write the test cases
against the harness from R4 *before* implementing the receiver:

- `../etc/passwd`, `/etc/passwd`, `foo/../bar`, `foo/./bar`,
  `foo\..\bar` (Windows-style separators), UTF-8 normalization tricks
  (`foo/\u002e\u002e/bar`), null bytes in names, names > 255 chars,
  nested depth > reasonable (e.g., 8 levels), total path > LittleFS
  limit (typically 63 or 127 chars per component).

LittleFS is forgiving about some of these and strict about others; better
to reject early than discover a bricked filesystem after a pack push.

### F3. Partition sizing

Main doc says ≥ 4 MB for LittleFS to stay over the 1.8 MB cap. Bump to
**≥ 8 MB** — the board has 32 MB of NOR, and giving yourself headroom for
multiple character packs coexisting (switchable via settings screen)
turns Phase 5 polish into a config change instead of a repartition.

---

## Phase 5 — Polish, ordered by ROI

Suggested priority within Phase 5, if time pressure hits:

1. **Settings screen** (species switcher, owner display, unpair button,
   pairing-mode toggle). Highest-impact for day-to-day use.
2. **Audio cues via ES8311** on approval prompts. Haptic substitute the
   main doc mentions. Short (~200 ms) distinctive chime, volume
   configurable in settings.
3. **Token-level celebrate animation.** Reference firmware does this on
   milestone boundaries; port the thresholds.
4. **Pets/animations.** `lv_gif` is the right call per §7, but this is
   the most time-sink-prone item in Phase 5 and the least functionally
   important. Timebox it.
5. **Stats history.** NVS-backed counters already exist from F1; surfacing
   them in UI is optional polish.

---

## Cross-cutting concerns

### C1. Power-loss behavior

USB-only, no battery, no hold-up cap worth mentioning. Design stance:

- NVS writes are write-through (ESP-IDF default). Any counter that matters
  should be flushed at update, not batched.
- No graceful shutdown path needed. On USB unplug the device dies;
  NimBLE peer will notice via supervision timeout; desktop will stop
  showing it live after ~30 s per `REFERENCE.md`.
- Document this explicitly in the firmware README so nobody wastes time
  implementing brownout detection.

### C2. `usb:true` with no PMIC — status ack shape

Main doc suggests omitting `mV/mA/pct`. Verify the desktop UI handles
omitted fields gracefully before shipping; if it renders "??" or blanks,
synthesize:

```json
"bat": { "pct": 100, "mV": 5000, "mA": 0, "usb": true }
```

Cosmetic only, but worth 15 minutes of checking against Claude Desktop's
actual rendering.

### C3. `idf_component.yml` version pinning + `HARDWARE.md`

Pin exact versions of:

- `espressif/esp_hosted_mcu` (the one that matches the C6 firmware
  observed on the shipped Waveshare unit — capability mask `0xd`).
- `espressif/esp_lcd_jd9365`
- `espressif/esp_lcd_touch_gt911`
- `lvgl/lvgl`

Plus a `HARDWARE.md` alongside source recording:

- C6 firmware version string from the `esp-hosted` banner at boot
- Observed panel LCD ID (`93 65 04`) and touch ID (`0x39 0x39 0x39`)
- MTU actually negotiated against macOS Claude Desktop from R1
- Any pin remapping vs. the stock Waveshare schematic

Future-you in six months wants to know whether a regression is the board,
the managed component, or the firmware.

### C4. Repo location + license

Personal GitHub, MIT license to match the reference repo's likely posture. Push from `privclaude` Claude Code
profile, not the Imagin default. If Anthropic wants to reference
alternative hardware targets from the official repo later, the license
and attribution need to be clean from commit 1.

### C5. Reusable round-display LVGL primitives

The round-safe layout primitives (circular clip, central safe-area rect,
arc-follow text, half-circle tap targets, perimeter countdown arc) are
worth factoring into a standalone component — `components/lv_round_ui/`
or similar — rather than embedding in `ui/screens/`.

Rationale: the 608D dashboard indicator work and any future driver-facing
tile for the bus will want the same primitives. Building them portable
from day one costs nothing extra.

---

## Summary — what changes, what doesn't

**Doesn't change:** phase order, phase scope, ESP-IDF + NimBLE + LVGL
architecture, layered code structure, all the §4 research findings on
hosted-HCI.

**Does change:**

- R1 MTU measurement is a formal gate between Phase 2 and Phase 3.
- Approval UX hardens to hold-to-confirm + default-deny before Phase 3
  ships, not in Phase 5 polish.
- NVS schema (F1) and LittleFS partition size (F3) decided before Phase 4
  starts writing.
- Reusable round-UI primitives (C5) factored out so the 608D dashboard
  work inherits them.
- Version pinning + `HARDWARE.md` (C3) added alongside source.