#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "cJSON.h"

/* Folder-push (Phase 4) state machine. Claude Desktop streams the
 * contents of a dropped folder as `char_begin` → repeated (`file` +
 * `chunk`* + `file_end`) → `char_end`. We persist into SPIFFS at
 * /spiffs/assets/<folder-name>/<relative-path>.
 *
 * The functions below return true to say "I handled this cmd and
 * you should ack accordingly" plus set *ok + *bytes_written for the
 * outgoing ack payload. Return false means the cmd isn't ours. */

esp_err_t asset_push_init(void);

bool asset_push_handle_char_begin(const cJSON *obj, bool *ok, int *n);
bool asset_push_handle_file      (const cJSON *obj, bool *ok, int *n);
bool asset_push_handle_chunk     (const cJSON *obj, bool *ok, int *n);
bool asset_push_handle_file_end  (const cJSON *obj, bool *ok, int *n);
bool asset_push_handle_char_end  (const cJSON *obj, bool *ok, int *n);
