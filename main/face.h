#pragma once

#include <stdbool.h>

#include "lvgl.h"

typedef enum {
    FACE_STATE_SLEEP,
    FACE_STATE_IDLE,
    FACE_STATE_BUSY,
    FACE_STATE_ATTENTION,
    FACE_STATE_APPROVED,
    FACE_STATE_DENIED,
    FACE_STATE_CELEBRATE,
} face_state_t;

void face_build(lv_obj_t *parent);
void face_set_state(face_state_t state);

/* Short single-line message shown above the aperture. Empty string
 * hides the label. Caller keeps ownership of the string. */
void face_set_message(const char *msg);

/* Like face_set_message but the label auto-clears after the given
 * duration. Useful for transient confirmations like "connected" that
 * shouldn't stick around. */
void face_set_message_transient(const char *msg, uint32_t ms);

/* Decision the user can pick during FACE_STATE_ATTENTION. Claude
 * Desktop's REFERENCE.md documents only "once" and "deny" and rejects
 * anything else, so we stick to those two. */
typedef enum {
    FACE_DECISION_DENY,
    FACE_DECISION_ONCE,
} face_decision_t;

typedef void (*face_decision_cb_t)(face_decision_t decision);
void face_set_decision_cb(face_decision_cb_t cb);

/* Secondary line shown during ATTENTION with the command/path the
 * desktop wants approval for. Empty string or NULL hides it. */
void face_set_hint(const char *hint);

/* Show a 6-digit pairing passkey prominently on the screen. Pass 0
 * to return to whatever state the face was in. While the passkey is
 * visible the normal aperture, message, hint, and decision buttons
 * are hidden so the user can't miss it. */
void face_show_passkey(uint32_t key);
