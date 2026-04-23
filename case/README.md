# Case / enclosure

STEP files for a physical enclosure around the Waveshare
ESP32-P4-WIFI6-Touch-LCD-3.4C board.

## `WaveShare-untested.step`

First-pass enclosure geometry. **Untested** — the file has not been
printed or test-fitted against the real board yet. Expect to iterate
on dimensions and tolerances after a first print.

### Design intent

- Sealed two-piece enclosure with a **pop-off back plane** (no screws,
  no standoffs) — the back snaps on and off for access to the board.
- No external cutouts for the reset/boot buttons or the SD card slot.
  Re-flashing goes through the USB-C port; the buttons are accessed
  only by popping the back off.
- The USB-C port is the single exposed interface.

### Printing

- **Supports are required** — the interior geometry has overhangs.
- Recommended starting parameters: 0.2 mm layer height, PETG or PLA+.
- Flip through the orientation in your slicer before sending — the
  visible-face surface quality is what carries the look.

### If you print it

Record what doesn't fit: board outline tolerances, USB-C port depth,
screen bezel clearance, speaker/mic openings, back-plane snap fit.
Open an issue or PR with photos + the tweak you made.

Once a revision has been verified on real hardware, the verified file
should drop the `-untested` suffix so downstream users can print it
with confidence.
