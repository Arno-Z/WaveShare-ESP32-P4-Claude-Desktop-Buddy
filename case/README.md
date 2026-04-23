# Case / enclosure

STEP files for a physical enclosure around the Waveshare
ESP32-P4-WIFI6-Touch-LCD-3.4C board.

## `WaveShare-untested.step`

First-pass enclosure geometry. **Untested** — the file has not been
printed or test-fitted against the real board yet. Expect to iterate
on the dimensions, button/port cutouts, and speaker chamber after a
first print.

If you print it:

- record what doesn't fit (board standoffs, USB-C port depth, touch
  screen bezel clearance, reset/boot button access, speaker opening,
  SD card slot if you use it),
- open an issue or PR with photos + the tweak you made.

Once a revision has been verified on real hardware, the verified file
should be named without the `-untested` suffix so downstream users can
print it with confidence.

## Printing notes (not yet validated)

- Recommended: 0.2 mm layer height, PETG or PLA+.
- No supports expected on a correctly-oriented print; flip test in
  your slicer before sending.
- Standoffs print with the board outline — thread-forming screws
  (M2 or M2.5) go directly into plastic.
