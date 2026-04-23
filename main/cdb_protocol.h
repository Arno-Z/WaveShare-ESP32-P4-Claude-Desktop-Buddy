#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Claude Desktop Buddy wire protocol: newline-delimited JSON over
 * NUS. This module owns the line framer, JSON parsing, and dispatch
 * of incoming messages to the face/touch modules. */

void cdb_protocol_init(void);

/* Byte sink — hand everything arriving on the NUS RX characteristic
 * to this function. Safe to call from the BLE task; dispatch runs on
 * a dedicated protocol task so the BLE host thread doesn't stall on
 * LVGL locks or JSON parsing. */
void cdb_protocol_rx_bytes(const uint8_t *data, size_t len);

/* Connection state hook. When the NUS link drops we clear the
 * pending-prompt state so touches can't approve stale decisions. */
void cdb_protocol_set_connected(bool connected);

/* Encryption state hook. Once the link is AES-CCM encrypted the
 * status ack reports sec=true. */
void cdb_protocol_set_encrypted(bool encrypted);

/* User decided a pending prompt via the touchscreen. The string is
 * copied into the outgoing `decision` field verbatim — Claude Desktop
 * documents "once" and "deny"; "always" is accepted by recent
 * builds. No-op if no prompt is currently pending. */
void cdb_protocol_decide_prompt(const char *decision);
