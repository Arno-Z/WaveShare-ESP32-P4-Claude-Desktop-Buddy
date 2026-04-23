#pragma once

#include "esp_err.h"

/* Bring up the I2S bus, ES8311 speaker codec, and the playback task.
 * Must be called after bsp_i2c_init (the BSP display init handles
 * that). Safe to call once at boot. */
esp_err_t audio_init(void);

/* Soft chime that fires when a permission prompt arrives. Returns
 * immediately; playback happens on a dedicated task. */
void audio_play_attention(void);

/* Bright ascending pluck when a permission is approved. */
void audio_play_approve(void);

/* Short muted descending tone when a permission is denied. */
void audio_play_deny(void);
