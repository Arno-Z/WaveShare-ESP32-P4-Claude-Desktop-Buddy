#include "asset_push.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "esp_log.h"
#include "mbedtls/base64.h"

#include "bsp/esp-bsp.h"

static const char *TAG = "assets";

/* Claude Desktop doc: "Events that serialize larger than 4KB are
 * dropped" — the chunks arriving on the wire are sized to keep each
 * JSON frame under that. The max raw-bytes chunk after base64-decoding
 * is therefore ~3 KB. We give ourselves plenty of headroom. */
#define MAX_B64_CHUNK    4096
#define MAX_RAW_CHUNK    3072
#define ASSETS_ROOT      "/spiffs/assets"
#define MAX_PATH         160

static bool   s_char_open;            /* char_begin seen, char_end not yet */
static bool   s_file_open;            /* file started, file_end not yet */
static char   s_char_dir[MAX_PATH];   /* /spiffs/assets/<charname> */
static char   s_file_path[MAX_PATH];  /* current file full path */
static FILE  *s_fp;
static int    s_file_bytes;           /* bytes written to current file */
static int    s_char_bytes;           /* bytes written across all files */

/* ----- helpers -------------------------------------------------- */

static bool is_safe_segment(const char *s)
{
    if (!s || !*s) return false;
    if (strcmp(s, ".") == 0 || strcmp(s, "..") == 0) return false;
    for (const char *p = s; *p; ++p) {
        if (*p == '/' || *p == '\\') return false;
        if ((unsigned char)*p < 0x20) return false;
    }
    return true;
}

/* Sanitise a folder name down to something safe for the FS: keep
 * [A-Za-z0-9._-], substitute everything else with '_'. Clamp length. */
static void sanitise_name(const char *in, char *out, size_t out_size)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < out_size && j < 48; ++i) {
        char c = in[i];
        if (isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-') {
            out[j++] = c;
        } else {
            out[j++] = '_';
        }
    }
    out[j] = '\0';
    if (j == 0) strncpy(out, "unnamed", out_size);
}

static bool build_file_path(const char *rel, char *out, size_t out_size)
{
    /* Reject absolute paths and path traversal. Split the relative
     * path on '/' and validate each segment. */
    if (!rel || !*rel) return false;
    if (rel[0] == '/' || rel[0] == '\\') return false;
    if (strstr(rel, "..")) return false;

    /* Walk segments; each one must pass is_safe_segment. */
    const char *p = rel;
    const char *start = p;
    while (*p) {
        if (*p == '/' || *p == '\\') {
            size_t len = p - start;
            char seg[64];
            if (len == 0 || len >= sizeof(seg)) return false;
            memcpy(seg, start, len);
            seg[len] = '\0';
            if (!is_safe_segment(seg)) return false;
            start = p + 1;
        }
        p++;
    }
    if (!is_safe_segment(start)) return false;

    int n = snprintf(out, out_size, "%s/%s", s_char_dir, rel);
    return n > 0 && (size_t)n < out_size;
}

static void mkdirs_for(const char *path)
{
    /* Create parent directories one segment at a time. SPIFFS is
     * flat (mkdir is effectively a no-op) but calling it is
     * harmless and portable. */
    char tmp[MAX_PATH];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

static void close_current_file(void)
{
    if (s_fp) { fclose(s_fp); s_fp = NULL; }
    s_file_open = false;
    s_file_path[0] = '\0';
    s_file_bytes = 0;
}

static void reset_all(void)
{
    close_current_file();
    s_char_open = false;
    s_char_dir[0] = '\0';
    s_char_bytes = 0;
}

/* ----- public init --------------------------------------------- */

esp_err_t asset_push_init(void)
{
    esp_err_t err = bsp_spiffs_mount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spiffs mount: %s", esp_err_to_name(err));
        return err;
    }
    /* Ensure the top-level assets/ exists. */
    mkdir(ASSETS_ROOT, 0755);
    return ESP_OK;
}

/* ----- protocol handlers --------------------------------------- */

bool asset_push_handle_char_begin(const cJSON *obj, bool *ok, int *n)
{
    /* Accept: start a fresh character folder. Any in-progress char
     * gets discarded — the desktop re-sends on timeout. */
    reset_all();

    const cJSON *name = cJSON_GetObjectItem(obj, "name");
    char safe[64] = {0};
    if (name && cJSON_IsString(name)) {
        sanitise_name(name->valuestring, safe, sizeof(safe));
    } else {
        strncpy(safe, "unnamed", sizeof(safe));
    }
    snprintf(s_char_dir, sizeof(s_char_dir), "%s/%s", ASSETS_ROOT, safe);
    mkdir(s_char_dir, 0755);
    s_char_open = true;
    s_char_bytes = 0;

    ESP_LOGI(TAG, "char_begin name=%s dir=%s", safe, s_char_dir);
    *ok = true;
    *n = 0;
    return true;
}

bool asset_push_handle_file(const cJSON *obj, bool *ok, int *n)
{
    *ok = false;
    *n = 0;
    if (!s_char_open) {
        ESP_LOGW(TAG, "file without char_begin");
        return true;
    }
    close_current_file();

    const cJSON *path_j = cJSON_GetObjectItem(obj, "path");
    if (!path_j || !cJSON_IsString(path_j)) return true;

    if (!build_file_path(path_j->valuestring, s_file_path,
                         sizeof(s_file_path))) {
        ESP_LOGW(TAG, "rejected path: %s", path_j->valuestring);
        return true;
    }
    mkdirs_for(s_file_path);

    s_fp = fopen(s_file_path, "wb");
    if (!s_fp) {
        ESP_LOGW(TAG, "fopen(%s): %s", s_file_path, strerror(errno));
        return true;
    }
    s_file_open = true;
    s_file_bytes = 0;
    ESP_LOGI(TAG, "file open: %s", s_file_path);
    *ok = true;
    return true;
}

bool asset_push_handle_chunk(const cJSON *obj, bool *ok, int *n)
{
    *ok = false;
    *n = s_file_bytes;
    if (!s_file_open || !s_fp) return true;

    const cJSON *d = cJSON_GetObjectItem(obj, "d");
    if (!d || !cJSON_IsString(d)) return true;
    const char *b64 = d->valuestring;
    size_t b64_len = strlen(b64);
    if (b64_len == 0 || b64_len > MAX_B64_CHUNK) return true;

    unsigned char out[MAX_RAW_CHUNK];
    size_t out_len = 0;
    int rc = mbedtls_base64_decode(out, sizeof(out), &out_len,
                                   (const unsigned char *)b64, b64_len);
    if (rc != 0) {
        ESP_LOGW(TAG, "base64 rc=%d b64_len=%zu", rc, b64_len);
        return true;
    }
    if (out_len > 0 && fwrite(out, 1, out_len, s_fp) != out_len) {
        ESP_LOGW(TAG, "fwrite failed: %s", strerror(errno));
        return true;
    }
    s_file_bytes += (int)out_len;
    s_char_bytes += (int)out_len;
    *ok = true;
    *n = s_file_bytes;
    return true;
}

bool asset_push_handle_file_end(const cJSON *obj, bool *ok, int *n)
{
    *n = s_file_bytes;
    if (!s_file_open) { *ok = false; return true; }
    if (s_fp) { fflush(s_fp); fclose(s_fp); s_fp = NULL; }
    ESP_LOGI(TAG, "file close: %s (%d B)", s_file_path, s_file_bytes);
    close_current_file();
    *ok = true;
    return true;
}

bool asset_push_handle_char_end(const cJSON *obj, bool *ok, int *n)
{
    *n = s_char_bytes;
    if (!s_char_open) { *ok = false; return true; }
    ESP_LOGI(TAG, "char_end %s total=%dB", s_char_dir, s_char_bytes);
    reset_all();
    *ok = true;
    return true;
}
