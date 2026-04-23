# Design notes — the face on the round display

The 800×800 round display is Claude's face. These are the intentional
choices behind it, so the code and the feel stay coherent as the
project grows.

## Principles

1. **One shape, many states.** A single central element — the *aperture* —
   morphs to express state. Not a cartoon face. Not a pet. Something
   closer to a lens, an iris, or an ember.
2. **Nothing in the corners.** The display is a circle. Compose for a
   circle. Text arcs or sits in a ≤ 560 px central rectangle.
3. **Breathing > blinking.** Motion is continuous and slow by default.
   Sharp motion signals attention or urgency — use it sparingly.
4. **Warm, not loud.** Anthropic-palette warm orange on black. Other
   colors only to carry specific meaning (red = needs you, green =
   approved, rose = heart).
5. **Touch is confirmatory, not directional.** The device isn't a
   phone. Users don't navigate menus. The main interaction is
   approving/denying an action — one gesture, unambiguous.

## The aperture

A centered element with three layers:

- **Core** — a soft gradient disc. Hue, saturation, radius vary with
  state. This is "the eye."
- **Inner ring** — a thin concentric ring, animates with activity
  (rotation, pulse frequency).
- **Outer arc** — sits just inside the bezel. Long-running-progress
  indicator; also the approval countdown ring.

## States

Mapped from the Buddy protocol (`running`, `waiting`, etc.):

| State         | Trigger                                  | Aperture                                           | Color          |
| ------------- | ---------------------------------------- | -------------------------------------------------- | -------------- |
| `sleep`       | BLE disconnected                         | Closed (thin horizontal line), slow dim pulse      | Dim warm gray  |
| `idle`        | Connected, nothing running               | Open, breathing at 0.25 Hz                         | Warm orange    |
| `busy`        | ≥ 1 session generating                   | Slightly tighter, breathing at 0.6 Hz              | Brighter orange |
| `attention`   | Permission prompt pending                | Wide open, rapid pulse, outer arc pulses fully    | Amber → red    |
| `approved`    | User tapped approve                      | Bloom + contract; rose particles briefly           | Rose           |
| `denied`      | User tapped deny                         | Quick fade to near-closed                          | Cool gray      |
| `celebrate`   | Milestone (e.g. 50K output tokens)       | Corona ripples                                     | Gold           |

## Approval flow

The moment that matters most.

- **Screen:** full-screen takeover. Aperture moves to upper third.
- **Middle:** tool name in a clean sans (e.g. `Bash`), one line of
  hint below (e.g. `git push origin main`), wrapped to the safe
  rectangle.
- **Bottom third:** two stacked half-circle regions:
  - Top half-circle → **Approve** (green tint, requires **swipe up**
    from center, not a tap).
  - Bottom half-circle → **Deny** (red tint, **swipe down**).
- **Outer ring:** 1-second countdown before gestures are armed, to
  prevent accidental approve on screen-wake.

Swipe instead of tap reduces accidental approvals and maps naturally to
"send up / send away."

## Sleep / wake

- Screen dims after 30 s idle (unless an approval is pending).
- Any touch wakes it. No dedicated power button; the long-press on the
  chassis side button (if wired) is a hard reset, nothing softer.

## Typography

- Body / hints: `Montserrat 20` from LVGL's built-in set. Clean,
  recognizable at distance.
- Tool name: `Montserrat 26 bold`.
- Rare status text: Montserrat 16.
- No custom fonts in Phase 0; we revisit if the geometric feel
  doesn't carry.

## What we're explicitly not doing

- No menu system / settings UI on-device. Settings go through NVS +
  the desktop app's status pane.
- No transcript scrolling UI. The latest `msg` is enough; we're not
  rebuilding the chat on the face.
- No emoji, no cartoon mascot. Character packs from upstream Buddy
  are a Phase 4+ affordance, not the primary identity.

## Why this design

The display is small, round, and at arm's length on a desk. It wants
to be a *presence*, not a UI. Pretending it's a phone or a pet
costs dignity without adding function. A lens-like, breathing focal
point conveys "I'm here, I'm working" with almost no ink, and
escalates cleanly when it needs attention.
