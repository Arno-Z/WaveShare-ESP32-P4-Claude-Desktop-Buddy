#include "audio.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_codec_dev.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "audio";

/* Audio format — the BSP's bsp_audio_init(NULL) default is 22050 Hz
 * mono 16-bit. We match it. */
#define AUDIO_SR  22050

static esp_codec_dev_handle_t s_codec;
static TaskHandle_t           s_task;

/* Bit flags posted via task notifications. */
#define EV_ATTENTION (1u << 0)

/* Short bell-like chime: two overlapping sine tones (major third) with
 * an exponential decay envelope. Sounds like a soft "ding" rather than
 * a beep. ~300 ms total, 16-bit PCM. */
static void
render_attention_chime(int16_t *buf, size_t n_samples)
{
    const float f1 = 880.0f;    /* A5  */
    const float f2 = 1108.73f;  /* C#6 — a major third above A5 */
    const float decay = 6.5f;   /* higher = shorter tail */
    const float gain  = 0.32f;

    for (size_t i = 0; i < n_samples; ++i) {
        float t   = (float)i / (float)AUDIO_SR;
        float env = expf(-decay * t);
        float s   = (sinf(2.0f * (float)M_PI * f1 * t) +
                     sinf(2.0f * (float)M_PI * f2 * t)) * 0.5f;
        float v   = s * env * gain;
        if (v >  0.98f) v =  0.98f;
        if (v < -0.98f) v = -0.98f;
        buf[i] = (int16_t)(v * 32767.0f);
    }
}

static void
audio_task(void *arg)
{
    const size_t n = (size_t)(AUDIO_SR * 0.3f);   /* 300 ms */
    int16_t *buf = (int16_t *)heap_caps_malloc(n * sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "chime buffer alloc failed");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        uint32_t notify = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notify, portMAX_DELAY);
        if (notify & EV_ATTENTION) {
            render_attention_chime(buf, n);
            int rc = esp_codec_dev_write(s_codec, buf, (int)(n * sizeof(int16_t)));
            if (rc != 0) {
                ESP_LOGW(TAG, "codec_write rc=%d", rc);
            }
        }
    }
}

esp_err_t
audio_init(void)
{
    esp_err_t err = bsp_audio_init(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init: %s", esp_err_to_name(err));
        return err;
    }
    s_codec = bsp_audio_codec_speaker_init();
    if (!s_codec) {
        ESP_LOGE(TAG, "speaker codec init failed");
        return ESP_FAIL;
    }
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 1,
        .sample_rate     = AUDIO_SR,
    };
    int rc = esp_codec_dev_open(s_codec, &fs);
    if (rc != 0) {
        ESP_LOGE(TAG, "codec_open rc=%d", rc);
        return ESP_FAIL;
    }
    /* Speaker volume — conservative to avoid startling the user on
     * first connect. Tune after listening tests. */
    esp_codec_dev_set_out_vol(s_codec, 55);

    BaseType_t ok = xTaskCreate(audio_task, "audio", 4096, NULL, 5, &s_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void
audio_play_attention(void)
{
    if (!s_task) return;
    xTaskNotify(s_task, EV_ATTENTION, eSetBits);
}
