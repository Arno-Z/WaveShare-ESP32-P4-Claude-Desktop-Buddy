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

/* Pre-allocated render buffer. Big enough for our longest chime
 * (~350 ms @ 22050 Hz = 7700 samples). */
#define AUDIO_MAX_SAMPLES  8192

static esp_codec_dev_handle_t s_codec;
static TaskHandle_t           s_task;
static int16_t               *s_buf;

/* Bit flags posted via task notifications. */
#define EV_ATTENTION (1u << 0)
#define EV_APPROVE   (1u << 1)
#define EV_DENY      (1u << 2)

/* A chime is two sine tones under a shared exponential decay envelope.
 * A second tone of 0 Hz yields a single-tone chime. */
typedef struct {
    float    f1;          /* primary tone, Hz */
    float    f2;          /* second tone (0 = mono-tone) */
    float    duration_s;  /* how long to render */
    float    decay;       /* exp decay rate; bigger = shorter tail */
    float    gain;        /* 0..1 peak gain pre-clip */
} chime_t;

static const chime_t CHIME_ATTENTION = {   /* major third "ding" */
    .f1 = 880.0f, .f2 = 1108.73f,
    .duration_s = 0.30f, .decay = 6.5f, .gain = 0.55f,
};
static const chime_t CHIME_APPROVE = {     /* bright high pluck */
    .f1 = 1318.51f, .f2 = 1760.0f,          /* E6 + A6 */
    .duration_s = 0.18f, .decay = 11.0f, .gain = 0.60f,
};
static const chime_t CHIME_DENY = {        /* low muted thud */
    .f1 = 196.0f, .f2 = 146.83f,            /* G3 + D3 */
    .duration_s = 0.25f, .decay = 9.0f, .gain = 0.65f,
};

/* Render a chime into the static buffer and return sample count. */
static size_t
render_chime(const chime_t *c)
{
    size_t n = (size_t)(AUDIO_SR * c->duration_s);
    if (n > AUDIO_MAX_SAMPLES) n = AUDIO_MAX_SAMPLES;

    const bool two_tone = c->f2 > 0.0f;
    const float mix = two_tone ? 0.5f : 1.0f;

    for (size_t i = 0; i < n; ++i) {
        float t   = (float)i / (float)AUDIO_SR;
        float env = expf(-c->decay * t);
        float s   = sinf(2.0f * (float)M_PI * c->f1 * t);
        if (two_tone) s += sinf(2.0f * (float)M_PI * c->f2 * t);
        float v = s * mix * env * c->gain;
        if (v >  0.98f) v =  0.98f;
        if (v < -0.98f) v = -0.98f;
        s_buf[i] = (int16_t)(v * 32767.0f);
    }
    return n;
}

static void
play_chime(const chime_t *c)
{
    size_t n = render_chime(c);
    int rc = esp_codec_dev_write(s_codec, s_buf, (int)(n * sizeof(int16_t)));
    if (rc != 0) ESP_LOGW(TAG, "codec_write rc=%d", rc);
}

static void
audio_task(void *arg)
{
    while (1) {
        uint32_t notify = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notify, portMAX_DELAY);
        /* Drain in priority order: a deny/approve almost always
         * follows an attention, so play deny/approve last. */
        if (notify & EV_ATTENTION) play_chime(&CHIME_ATTENTION);
        if (notify & EV_APPROVE)   play_chime(&CHIME_APPROVE);
        if (notify & EV_DENY)      play_chime(&CHIME_DENY);
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
    /* Speaker volume — the ES8311 is pretty quiet at lower settings
     * on this module; 85 is audible from a desk without being a
     * startle. */
    esp_codec_dev_set_out_vol(s_codec, 85);

    s_buf = (int16_t *)heap_caps_malloc(AUDIO_MAX_SAMPLES * sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGE(TAG, "chime buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(audio_task, "audio", 4096, NULL, 5, &s_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static inline void notify(uint32_t bit) {
    if (s_task) xTaskNotify(s_task, bit, eSetBits);
}

void audio_play_attention(void) { notify(EV_ATTENTION); }
void audio_play_approve(void)   { notify(EV_APPROVE);   }
void audio_play_deny(void)      { notify(EV_DENY);      }
