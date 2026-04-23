#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*ble_nus_rx_cb_t)(const uint8_t *data, size_t len);
typedef void (*ble_nus_conn_cb_t)(bool connected);
/* Passkey to display on the device screen during pairing. A value of
 * 0 means "pairing ended, hide the passkey". */
typedef void (*ble_nus_passkey_cb_t)(uint32_t passkey);
typedef void (*ble_nus_encrypted_cb_t)(bool encrypted);

typedef struct {
    const char             *device_name;
    ble_nus_rx_cb_t         on_rx;
    ble_nus_conn_cb_t       on_conn;
    ble_nus_passkey_cb_t    on_passkey;   /* nullable */
    ble_nus_encrypted_cb_t  on_encrypted; /* nullable */
} ble_nus_cfg_t;

esp_err_t ble_nus_start(const ble_nus_cfg_t *cfg);

/* Notify the subscribed central over the TX characteristic.
 * Safe to call from any task; returns ESP_ERR_INVALID_STATE when not
 * connected or not subscribed. */
esp_err_t ble_nus_send(const uint8_t *data, size_t len);

/* Erase every stored bond. If a central is currently connected it
 * will be disconnected. Next incoming connection must re-pair. */
esp_err_t ble_nus_unpair_all(void);
