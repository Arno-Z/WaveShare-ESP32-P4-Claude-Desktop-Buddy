#include "face.h"

#include <stdio.h>

#include "lvgl.h"

#define DISPLAY_DIAMETER 800
#define CENTER           (DISPLAY_DIAMETER / 2)

#define APERTURE_MIN     140
#define APERTURE_MAX     260
#define APERTURE_BREATH_MS 3800  // slow idle breathing

static lv_obj_t *g_aperture_outer;
static lv_obj_t *g_aperture_core;
static lv_obj_t *g_msg_label;
static lv_obj_t *g_hint_label;
static lv_obj_t *g_btn_deny;
static lv_obj_t *g_btn_approve;
static lv_obj_t *g_passkey_panel;
static lv_obj_t *g_passkey_label;
static lv_obj_t *g_passkey_caption;

static lv_anim_t  g_breath_anim;
static lv_timer_t *g_msg_fade_timer;

/* Cross-fade state — LVGL anims are stateful per (object, exec_cb)
 * pair, so a single source/target pair is safe as long as we don't
 * start two fades in parallel. */
static lv_color_t g_cur_color;
static lv_color_t g_fade_from;
static lv_color_t g_fade_to;

static face_decision_cb_t g_decision_cb;

static void aperture_size_cb(void *obj, int32_t v)
{
    lv_obj_set_size((lv_obj_t *)obj, v, v);
}

static void build_background(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
}

static lv_obj_t *make_disc(lv_obj_t *parent, int32_t size, lv_color_t color, lv_opa_t opa)
{
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, size, size);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, color, 0);
    lv_obj_set_style_bg_opa(d, opa, 0);
    lv_obj_center(d);
    return d;
}

static void decision_click_cb(lv_event_t *e)
{
    face_decision_t d = (face_decision_t)(intptr_t)lv_event_get_user_data(e);
    if (g_decision_cb) g_decision_cb(d);
}

/* One of the two ATTENTION-state buttons. Deny (red) sits below-left
 * of the aperture, Approve (green) below-right. */
static lv_obj_t *
make_decision_button(lv_obj_t *parent, face_decision_t which)
{
    bool approve = (which == FACE_DECISION_ONCE);
    const int32_t size = 200;
    const int32_t x    = approve ? +160 : -160;
    const int32_t y    = +220;
    const uint32_t bg  = approve ? 0x2E7D4E : 0xB24040;
    const char *label  = approve ? "Approve" : "Deny";

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(btn, 30, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_50, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_align(btn, LV_ALIGN_CENTER, x, y);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_26, 0);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, decision_click_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)which);
    return btn;
}

void face_build(lv_obj_t *parent)
{
    build_background(parent);

    // Outer ring — thin halo behind the core. Gives the aperture depth.
    g_aperture_outer = make_disc(parent, APERTURE_MAX + 60,
                                 lv_color_hex(0xFF7A33), LV_OPA_30);

    // Core — the lens itself. Warm Anthropic-ish orange.
    g_aperture_core  = make_disc(parent, APERTURE_MIN,
                                 lv_color_hex(0xFF6B35), LV_OPA_COVER);
    /* Seed the cross-fade tracking so the first state change has a
     * sane "from" color to interpolate from. */
    g_cur_color = lv_color_hex(0xFF6B35);

    // Message line above the aperture so the decision buttons below
    // have the unobstructed bottom half of the round panel.
    g_msg_label = lv_label_create(parent);
    lv_label_set_long_mode(g_msg_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_msg_label, 620);
    lv_obj_set_style_text_align(g_msg_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_msg_label, lv_color_hex(0xE0D0B0), 0);
    lv_obj_set_style_text_font(g_msg_label, &lv_font_montserrat_26, 0);
    lv_label_set_text(g_msg_label, "");
    lv_obj_align(g_msg_label, LV_ALIGN_CENTER, 0, -230);

    g_btn_deny    = make_decision_button(parent, FACE_DECISION_DENY);
    g_btn_approve = make_decision_button(parent, FACE_DECISION_ONCE);

    // Passkey panel: a full-screen black overlay with a caption and
    // a very large centered 6-digit number. Hidden unless a pairing
    // attempt is in progress.
    g_passkey_panel = lv_obj_create(parent);
    lv_obj_remove_style_all(g_passkey_panel);
    lv_obj_set_size(g_passkey_panel, DISPLAY_DIAMETER, DISPLAY_DIAMETER);
    lv_obj_set_style_bg_color(g_passkey_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_passkey_panel, LV_OPA_COVER, 0);
    lv_obj_center(g_passkey_panel);
    lv_obj_add_flag(g_passkey_panel, LV_OBJ_FLAG_HIDDEN);
    // The overlay shouldn't steal touches — if a phantom click fires
    // during pairing we don't want it to propagate to the screen.
    lv_obj_add_flag(g_passkey_panel, LV_OBJ_FLAG_CLICKABLE);

    g_passkey_caption = lv_label_create(g_passkey_panel);
    lv_label_set_text(g_passkey_caption, "Pairing");
    lv_obj_set_style_text_color(g_passkey_caption, lv_color_hex(0xA89477), 0);
    lv_obj_set_style_text_font(g_passkey_caption, &lv_font_montserrat_26, 0);
    lv_obj_align(g_passkey_caption, LV_ALIGN_CENTER, 0, -110);

    g_passkey_label = lv_label_create(g_passkey_panel);
    lv_label_set_text(g_passkey_label, "000000");
    lv_obj_set_style_text_color(g_passkey_label, lv_color_hex(0xFFB340), 0);
    lv_obj_set_style_text_font(g_passkey_label, &lv_font_montserrat_48, 0);
    lv_obj_align(g_passkey_label, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t *hint = lv_label_create(g_passkey_panel);
    lv_label_set_text(hint, "enter this code on your Mac");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8A7A5C), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_20, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 60);

    // Secondary line used during ATTENTION to show prompt.hint (the
    // actual command / path). Dimmer than the main msg, Montserrat 20.
    g_hint_label = lv_label_create(parent);
    lv_label_set_long_mode(g_hint_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_hint_label, 620);
    lv_obj_set_style_text_align(g_hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_hint_label, lv_color_hex(0xA89477), 0);
    lv_obj_set_style_text_font(g_hint_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(g_hint_label, "");
    lv_obj_align(g_hint_label, LV_ALIGN_CENTER, 0, -175);

    face_set_state(FACE_STATE_IDLE);
}

void face_set_decision_cb(face_decision_cb_t cb)
{
    g_decision_cb = cb;
}

void face_set_message(const char *msg)
{
    if (!g_msg_label) return;
    if (g_msg_fade_timer) {
        lv_timer_delete(g_msg_fade_timer);
        g_msg_fade_timer = NULL;
    }
    lv_label_set_text(g_msg_label, msg ? msg : "");
}

static void start_breath(int32_t lo, int32_t hi, uint32_t period_ms)
{
    lv_anim_init(&g_breath_anim);
    lv_anim_set_var(&g_breath_anim, g_aperture_core);
    lv_anim_set_exec_cb(&g_breath_anim, aperture_size_cb);
    lv_anim_set_values(&g_breath_anim, lo, hi);
    lv_anim_set_duration(&g_breath_anim, period_ms / 2);
    lv_anim_set_playback_duration(&g_breath_anim, period_ms / 2);
    lv_anim_set_repeat_count(&g_breath_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&g_breath_anim, lv_anim_path_ease_in_out);
    lv_anim_start(&g_breath_anim);
}

/* Cross-fade helpers. The exec callback is the second arg to
 * lv_anim_set_exec_cb; LVGL keys active anims on (var, exec_cb) so we
 * need a named function rather than a lambda. */
static void color_fade_cb(void *obj, int32_t v)
{
    lv_color_t c = lv_color_mix(g_fade_to, g_fade_from, (uint8_t)v);
    lv_obj_set_style_bg_color((lv_obj_t *)obj, c, 0);
}

static void start_color_fade(lv_obj_t *obj, uint32_t hex_to, uint32_t ms)
{
    g_fade_from = g_cur_color;
    g_fade_to   = lv_color_hex(hex_to);
    g_cur_color = g_fade_to;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, color_fade_cb);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* Single-shot size tween with playback overshoot — used for the
 * APPROVED bloom. Starts from the aperture's current size. */
static void start_bloom(int32_t peak, int32_t settle, uint32_t rise_ms, uint32_t fall_ms)
{
    lv_anim_delete(g_aperture_core, aperture_size_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_aperture_core);
    lv_anim_set_exec_cb(&a, aperture_size_cb);
    lv_anim_set_values(&a, settle, peak);
    lv_anim_set_duration(&a, rise_ms);
    lv_anim_set_playback_duration(&a, fall_ms);
    lv_anim_set_repeat_count(&a, 1);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void show_decision_buttons(bool show)
{
    lv_obj_t *btns[] = { g_btn_deny, g_btn_approve };
    for (size_t i = 0; i < sizeof(btns) / sizeof(btns[0]); ++i) {
        if (!btns[i]) continue;
        if (show) lv_obj_remove_flag(btns[i], LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag   (btns[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void face_set_hint(const char *hint)
{
    if (!g_hint_label) return;
    lv_label_set_text(g_hint_label, hint ? hint : "");
}

static void msg_fade_cb(lv_timer_t *t)
{
    lv_label_set_text(g_msg_label, "");
    g_msg_fade_timer = NULL;
    lv_timer_delete(t);
}

void face_set_message_transient(const char *msg, uint32_t ms)
{
    face_set_message(msg);
    if (g_msg_fade_timer) {
        lv_timer_delete(g_msg_fade_timer);
        g_msg_fade_timer = NULL;
    }
    if (msg && msg[0] && ms > 0) {
        g_msg_fade_timer = lv_timer_create(msg_fade_cb, ms, NULL);
        lv_timer_set_repeat_count(g_msg_fade_timer, 1);
    }
}

void face_show_passkey(uint32_t key)
{
    if (!g_passkey_panel) return;
    if (key == 0) {
        lv_obj_add_flag(g_passkey_panel, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    char buf[8];
    // The spec caps at 6 digits. Format with leading zeros.
    snprintf(buf, sizeof(buf), "%06u", (unsigned)(key % 1000000));
    lv_label_set_text(g_passkey_label, buf);
    lv_obj_remove_flag(g_passkey_panel, LV_OBJ_FLAG_HIDDEN);
    // Make sure we're on top of the aperture/buttons.
    lv_obj_move_foreground(g_passkey_panel);
}

void face_set_state(face_state_t state)
{
    if (!g_aperture_core) return;

    show_decision_buttons(state == FACE_STATE_ATTENTION);

    switch (state) {
    case FACE_STATE_SLEEP:
        lv_anim_delete(g_aperture_core, aperture_size_cb);
        lv_obj_set_size(g_aperture_core, APERTURE_MIN, 4);  // closed slit
        start_color_fade(g_aperture_core, 0x5A3A20, 350);
        break;
    case FACE_STATE_IDLE:
        start_color_fade(g_aperture_core, 0xFF6B35, 350);
        start_breath(APERTURE_MIN, APERTURE_MAX, APERTURE_BREATH_MS);
        break;
    case FACE_STATE_BUSY:
        start_color_fade(g_aperture_core, 0xFFA04A, 220);
        start_breath(APERTURE_MIN + 20, APERTURE_MAX, APERTURE_BREATH_MS / 3);
        break;
    case FACE_STATE_ATTENTION:
        /* Fast-ish transition so the user feels the urgency. */
        start_color_fade(g_aperture_core, 0xFFB340, 180);
        start_breath(APERTURE_MAX - 30, APERTURE_MAX + 30, 900);
        break;
    case FACE_STATE_APPROVED:
        /* Pink bloom overshoot — grows past full size then settles. */
        start_color_fade(g_aperture_core, 0xE88FB0, 140);
        start_bloom(APERTURE_MAX + 30, APERTURE_MAX, 140, 260);
        break;
    case FACE_STATE_DENIED:
        /* Quick contract + muted grey. */
        lv_anim_delete(g_aperture_core, aperture_size_cb);
        lv_obj_set_size(g_aperture_core, APERTURE_MIN, APERTURE_MIN);
        start_color_fade(g_aperture_core, 0x6B6B6B, 180);
        break;
    case FACE_STATE_CELEBRATE:
        start_color_fade(g_aperture_core, 0xFFD66B, 300);
        start_breath(APERTURE_MIN, APERTURE_MAX + 40, 700);
        break;
    }
}
