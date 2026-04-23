#include "cdb_protocol.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "cJSON.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"

#include "asset_push.h"
#include "audio.h"
#include "ble_nus.h"
#include "face.h"

static const char *TAG = "cdb";

/* RX framer state. 2 KB is comfortably larger than anything the
 * desktop sends — heartbeats are typically ~200 bytes; the biggest
 * regular frames are chunk acks (~400 bytes base64). The folder-push
 * protocol streams files in separate chunk frames, so no single line
 * ever approaches 1 MB. */
#define RX_BUF_MAX 2048
static uint8_t  s_rx[RX_BUF_MAX];
static size_t   s_rx_len;

/* Connection + pending-prompt state. */
static bool          s_connected;
static bool          s_encrypted;
static char          s_pending_id[48];
static bool          s_has_pending_prompt;

/* One-shot revert timer: after APPROVED/DENIED we flash that face
 * state for a short moment, then drop back to IDLE (or SLEEP if we
 * disconnected in the meantime). A follow-up heartbeat would also
 * transition us — the timer is there for fake_buddy testing and for
 * the case where the desktop never sends a fresh snapshot after the
 * decision. */
static lv_timer_t *s_revert_timer;
#define REVERT_DELAY_MS 600

/* ---- Line assembly → JSON parse ------------------------------- */

static void process_line(const char *line);

static void cancel_revert_timer(void)
{
    if (s_revert_timer) {
        lv_timer_delete(s_revert_timer);
        s_revert_timer = NULL;
    }
}

static void revert_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* Called inside LVGL's timer handler — mutex already held. */
    face_set_state(s_connected ? FACE_STATE_IDLE : FACE_STATE_SLEEP);
    face_set_hint("");
    face_set_message(s_connected ? "" : "");
    s_revert_timer = NULL;
    lv_timer_delete(t);
}

void cdb_protocol_init(void)
{
    s_rx_len = 0;
    s_connected = false;
    s_encrypted = false;
    s_has_pending_prompt = false;
    s_pending_id[0] = '\0';
    s_revert_timer = NULL;
}

void cdb_protocol_rx_bytes(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        if (b == '\n') {
            if (s_rx_len == 0) continue;            /* blank line */
            s_rx[s_rx_len] = '\0';
            process_line((const char *)s_rx);
            s_rx_len = 0;
        } else if (s_rx_len + 1 < RX_BUF_MAX) {
            s_rx[s_rx_len++] = b;
        } else {
            ESP_LOGW(TAG, "rx buffer overflow — dropping %u bytes", (unsigned)s_rx_len);
            s_rx_len = 0;                           /* resync on next '\n' */
        }
    }
}

/* ---- TX helpers ------------------------------------------------ */

static void send_line(const char *json_no_newline)
{
    size_t n = strlen(json_no_newline);
    /* Append '\n' without a second round-trip by allocating one buffer. */
    uint8_t buf[512];
    if (n + 1 > sizeof(buf)) {
        ESP_LOGW(TAG, "tx line too long (%u)", (unsigned)n);
        return;
    }
    memcpy(buf, json_no_newline, n);
    buf[n] = '\n';
    esp_err_t err = ble_nus_send(buf, n + 1);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "send %s: %d", json_no_newline, err);
    }
}

static void send_ack(const char *cmd, bool ok, int n)
{
    char buf[96];
    snprintf(buf, sizeof(buf),
             "{\"ack\":\"%s\",\"ok\":%s,\"n\":%d}",
             cmd, ok ? "true" : "false", n);
    send_line(buf);
}

/* ---- Handlers -------------------------------------------------- */

/* Heartbeat snapshot — no `cmd`, no `evt`. Map fields onto the face. */
static void
on_snapshot(const cJSON *obj)
{
    int total    = 0;
    int running  = 0;
    int waiting  = 0;
    const char *msg = NULL;

    cJSON *it;
    if ((it = cJSON_GetObjectItem(obj, "total"))   && cJSON_IsNumber(it)) total   = it->valueint;
    if ((it = cJSON_GetObjectItem(obj, "running")) && cJSON_IsNumber(it)) running = it->valueint;
    if ((it = cJSON_GetObjectItem(obj, "waiting")) && cJSON_IsNumber(it)) waiting = it->valueint;
    if ((it = cJSON_GetObjectItem(obj, "msg"))     && cJSON_IsString(it)) msg     = it->valuestring;

    cJSON *prompt = cJSON_GetObjectItem(obj, "prompt");
    const cJSON *prompt_id   = prompt ? cJSON_GetObjectItem(prompt, "id")   : NULL;
    const cJSON *prompt_hint = prompt ? cJSON_GetObjectItem(prompt, "hint") : NULL;
    const cJSON *prompt_tool = prompt ? cJSON_GetObjectItem(prompt, "tool") : NULL;

    ESP_LOGI(TAG, "snap total=%d running=%d waiting=%d prompt=%s msg=\"%s\"",
             total, running, waiting, prompt ? "yes" : "no",
             msg ? msg : "");

    /* Build a secondary "<tool>: <hint>" line for the face. Caller
     * keeps the string live until face_set_hint copies it. */
    char hint_buf[200] = "";
    if (prompt) {
        const char *tool = (prompt_tool && cJSON_IsString(prompt_tool)) ? prompt_tool->valuestring : NULL;
        const char *hint = (prompt_hint && cJSON_IsString(prompt_hint)) ? prompt_hint->valuestring : NULL;
        if (tool && hint)      snprintf(hint_buf, sizeof(hint_buf), "%s  %s", tool, hint);
        else if (hint)         snprintf(hint_buf, sizeof(hint_buf), "%s", hint);
        else if (tool)         snprintf(hint_buf, sizeof(hint_buf), "%s", tool);
    }

    face_state_t state;
    if (prompt && prompt_id && cJSON_IsString(prompt_id)) {
        strncpy(s_pending_id, prompt_id->valuestring, sizeof(s_pending_id) - 1);
        s_pending_id[sizeof(s_pending_id) - 1] = '\0';
        s_has_pending_prompt = true;
        state = FACE_STATE_ATTENTION;
    } else {
        s_has_pending_prompt = false;
        s_pending_id[0] = '\0';
        if (waiting > 0)       state = FACE_STATE_ATTENTION;
        else if (running > 0)  state = FACE_STATE_BUSY;
        else if (total == 0)   state = FACE_STATE_SLEEP;
        else                   state = FACE_STATE_IDLE;
    }

    /* Only show the one-liner during states where it actually adds
     * information. IDLE/SLEEP "N idle" / "nothing open" heartbeats
     * are noise — the aperture already conveys those states. */
    bool show_msg = (state == FACE_STATE_BUSY ||
                     state == FACE_STATE_ATTENTION);

    /* Audible chime whenever a NEW permission request lands. Gate on
     * the prompt id rather than the face state — a fresh prompt with
     * a new id should re-chime even if we were already in ATTENTION,
     * while repeated keepalives with the same id stay silent. */
    static char s_last_chimed_id[sizeof(s_pending_id)] = {0};
    if (state == FACE_STATE_ATTENTION &&
        s_has_pending_prompt &&
        strcmp(s_last_chimed_id, s_pending_id) != 0) {
        audio_play_attention();
        strncpy(s_last_chimed_id, s_pending_id, sizeof(s_last_chimed_id) - 1);
        s_last_chimed_id[sizeof(s_last_chimed_id) - 1] = '\0';
    } else if (state != FACE_STATE_ATTENTION) {
        /* Clear the id so the next ATTENTION (even same prompt) rings. */
        s_last_chimed_id[0] = '\0';
    }

    bsp_display_lock(-1);
    /* A fresh snapshot takes precedence over any pending auto-revert. */
    cancel_revert_timer();
    face_set_state(state);
    face_set_message(show_msg && msg ? msg : "");
    face_set_hint(hint_buf);
    bsp_display_unlock();
}

static void
on_cmd_status(void)
{
    /* Minimal status ack — enough to populate the desktop's stats
     * panel. Battery + task stats land in later phases. */
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"ack\":\"status\",\"ok\":true,\"data\":{"
                 "\"name\":\"Claude Face\","
                 "\"sec\":%s,"
                 "\"sys\":{\"up\":%lu,\"heap\":%lu}"
             "}}",
             s_encrypted ? "true" : "false",
             (unsigned long)(esp_log_timestamp() / 1000),
             (unsigned long)esp_get_free_heap_size());
    send_line(buf);
}

static void
dispatch(const cJSON *obj)
{
    const cJSON *cmd = cJSON_GetObjectItem(obj, "cmd");
    const cJSON *evt = cJSON_GetObjectItem(obj, "evt");

    if (cmd && cJSON_IsString(cmd)) {
        const char *name = cmd->valuestring;
        if (strcmp(name, "status") == 0) {
            on_cmd_status();
        } else if (strcmp(name, "name") == 0 ||
                   strcmp(name, "owner") == 0) {
            /* Accept; name/owner persistence lands with the preferences phase. */
            send_ack(name, true, 0);
        } else if (strcmp(name, "unpair") == 0) {
            esp_err_t err = ble_nus_unpair_all();
            send_ack("unpair", err == ESP_OK, 0);
        } else if (strcmp(name, "char_begin") == 0 ||
                   strcmp(name, "char_end") == 0 ||
                   strcmp(name, "file") == 0 ||
                   strcmp(name, "file_end") == 0 ||
                   strcmp(name, "chunk") == 0) {
            /* Phase 4 folder push — SPIFFS-backed. */
            bool ok = false;
            int  nbytes = 0;
            bool handled = false;
            if      (!strcmp(name, "char_begin")) handled = asset_push_handle_char_begin(obj, &ok, &nbytes);
            else if (!strcmp(name, "file"))       handled = asset_push_handle_file      (obj, &ok, &nbytes);
            else if (!strcmp(name, "chunk"))      handled = asset_push_handle_chunk     (obj, &ok, &nbytes);
            else if (!strcmp(name, "file_end"))   handled = asset_push_handle_file_end  (obj, &ok, &nbytes);
            else if (!strcmp(name, "char_end"))   handled = asset_push_handle_char_end  (obj, &ok, &nbytes);
            if (handled) send_ack(name, ok, nbytes);
        } else {
            ESP_LOGI(TAG, "unknown cmd: %s", name);
        }
        return;
    }

    if (evt && cJSON_IsString(evt)) {
        /* turn events — log only for now. */
        ESP_LOGD(TAG, "evt %s", evt->valuestring);
        return;
    }

    /* No cmd, no evt → heartbeat snapshot. */
    on_snapshot(obj);
}

static void
process_line(const char *line)
{
    ESP_LOGI(TAG, "<- %.300s", line);
    cJSON *obj = cJSON_Parse(line);
    if (!obj) {
        ESP_LOGW(TAG, "parse fail: %.120s", line);
        return;
    }
    dispatch(obj);
    cJSON_Delete(obj);
}

/* ---- Public hooks from other modules -------------------------- */

void cdb_protocol_set_connected(bool connected)
{
    s_connected = connected;
    if (!connected) {
        s_rx_len = 0;
        s_encrypted = false;
        s_has_pending_prompt = false;
        s_pending_id[0] = '\0';
        bsp_display_lock(-1);
        cancel_revert_timer();
        bsp_display_unlock();
    }
}

void cdb_protocol_set_encrypted(bool encrypted)
{
    s_encrypted = encrypted;
    ESP_LOGI(TAG, "link encrypted=%d", encrypted);
}

void cdb_protocol_decide_prompt(const char *decision)
{
    if (!s_connected || !s_has_pending_prompt) {
        ESP_LOGD(TAG, "decide ignored (connected=%d pending=%d)",
                 s_connected, s_has_pending_prompt);
        return;
    }
    if (!decision) decision = "deny";
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"%s\"}",
             s_pending_id, decision);
    send_line(buf);

    bool approved = (strcmp(decision, "deny") != 0);
    if (approved) audio_play_approve();
    else          audio_play_deny();
    bsp_display_lock(-1);
    face_set_state(approved ? FACE_STATE_APPROVED : FACE_STATE_DENIED);
    face_set_hint("");
    cancel_revert_timer();
    s_revert_timer = lv_timer_create(revert_timer_cb, REVERT_DELAY_MS, NULL);
    lv_timer_set_repeat_count(s_revert_timer, 1);
    bsp_display_unlock();

    /* Clear local state immediately — next heartbeat will correct us
     * if the desktop disagrees. */
    s_has_pending_prompt = false;
    s_pending_id[0] = '\0';
}
