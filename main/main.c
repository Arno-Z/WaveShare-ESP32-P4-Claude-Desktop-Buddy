#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "asset_push.h"
#include "audio.h"
#include "ble_nus.h"
#include "cdb_protocol.h"
#include "face.h"

static const char *TAG = "claude-face";

static void
on_ble_rx(const uint8_t *data, size_t len)
{
    cdb_protocol_rx_bytes(data, len);
}

static void
on_ble_conn(bool connected)
{
    ESP_LOGI(TAG, "BLE %s", connected ? "connected" : "disconnected");
    cdb_protocol_set_connected(connected);

    bsp_display_lock(-1);
    face_set_state(connected ? FACE_STATE_IDLE : FACE_STATE_SLEEP);
    face_set_hint("");
    if (connected) {
        /* Flash "connected" briefly — the aperture itself is the
         * lasting state indicator. */
        face_set_message_transient("connected", 2000);
    } else {
        face_set_message("not connected");
    }
    bsp_display_unlock();
}

static void
on_ble_passkey(uint32_t key)
{
    bsp_display_lock(-1);
    face_show_passkey(key);
    bsp_display_unlock();
}

static void
on_ble_encrypted(bool encrypted)
{
    cdb_protocol_set_encrypted(encrypted);
}

static void
on_decision(face_decision_t d)
{
    const char *s = (d == FACE_DECISION_ONCE) ? "once" : "deny";
    ESP_LOGI(TAG, "button -> %s", s);
    cdb_protocol_decide_prompt(s);
}

void app_main(void)
{
    ESP_LOGI(TAG, "booting Claude face (Phase 2)");

    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg   = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation         = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode  = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags      = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    if (audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "audio init failed — running silent");
    }
    if (asset_push_init() != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed — folder push disabled");
    }

    bsp_display_lock(-1);
    face_build(lv_screen_active());
    face_set_state(FACE_STATE_SLEEP);
    face_set_message("not connected");
    face_set_decision_cb(on_decision);
    bsp_display_unlock();

    cdb_protocol_init();

    ble_nus_cfg_t ble_cfg = {
        .device_name   = "Claude Face",
        .on_rx         = on_ble_rx,
        .on_conn       = on_ble_conn,
        .on_passkey    = on_ble_passkey,
        .on_encrypted  = on_ble_encrypted,
    };
    esp_err_t err = ble_nus_start(&ble_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed: %s", esp_err_to_name(err));
    }
}
