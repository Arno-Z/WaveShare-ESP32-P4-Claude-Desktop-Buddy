#include "ble_nus.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_random.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "esp_hosted.h"

static const char *TAG = "ble-nus";

/* Nordic UART Service UUIDs — Claude Desktop talks to this.
 * Text form: 6e400001/2/3-b5a3-f393-e0a9-e50e24dcca9e
 * NimBLE BLE_UUID128_INIT takes bytes in little-endian, so the
 * sequence below is the textual UUID reversed byte-wise. */
#define NUS_UUID128(last_byte_triplet)                                  \
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,    \
                     0x93, 0xf3, 0xa3, 0xb5, last_byte_triplet, 0x00, 0x40, 0x6e)

static const ble_uuid128_t nus_svc_uuid = NUS_UUID128(0x01);
static const ble_uuid128_t nus_rx_uuid  = NUS_UUID128(0x02);
static const ble_uuid128_t nus_tx_uuid  = NUS_UUID128(0x03);

/* Runtime state. */
static char                     s_device_name[32];
static ble_nus_rx_cb_t          s_on_rx;
static ble_nus_conn_cb_t        s_on_conn;
static ble_nus_passkey_cb_t     s_on_passkey;
static ble_nus_encrypted_cb_t   s_on_encrypted;
static uint8_t                  s_own_addr_type;
static uint16_t                 s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t                 s_tx_val_handle;
static bool                     s_tx_subscribed;

static int  nus_gap_event(struct ble_gap_event *event, void *arg);
static void nus_advertise(void);
static void ble_host_task(void *param);

void ble_store_config_init(void);

/* ----- GATT service table ---------------------------------------- *
 * Both characteristics require LE encryption on every access. That
 * propagates to the TX CCCD (the central must be bonded to subscribe)
 * and to RX writes (the central must be bonded to send anything). */

static int
nus_chr_access(uint16_t conn_handle, uint16_t attr_handle,
               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR &&
        ble_uuid_cmp(ctxt->chr->uuid, &nus_rx_uuid.u) == 0) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        ESP_LOGI(TAG, "RX write %uB", (unsigned)len);
        if (len == 0 || len > 512) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        uint8_t buf[512];
        uint16_t out_len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &out_len);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (s_on_rx) {
            s_on_rx(buf, out_len);
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def nus_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &nus_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &nus_rx_uuid.u,
                .access_cb = nus_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                         BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                .uuid = &nus_tx_uuid.u,
                .access_cb = nus_chr_access,
                /* F_WRITE_ENC is what NimBLE actually checks when the
                 * central writes to the auto-created CCCD. Without
                 * it, an unbonded peer can subscribe; with it, the
                 * CCCD write forces the pairing handshake. */
                .flags = BLE_GATT_CHR_F_NOTIFY |
                         BLE_GATT_CHR_F_READ_ENC |
                         BLE_GATT_CHR_F_WRITE_ENC,
                .val_handle = &s_tx_val_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ----- GAP advertising ------------------------------------------- */

static void
nus_advertise(void)
{
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    struct ble_hs_adv_fields fields = {0};
    struct ble_hs_adv_fields rsp_fields = {0};

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.uuids128 = (ble_uuid128_t *)&nus_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    rsp_fields.name = (uint8_t *)s_device_name;
    rsp_fields.name_len = strlen(s_device_name);
    rsp_fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_set_fields rc=%d", rc); return; }
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_rsp_set_fields rc=%d", rc); return; }
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, nus_gap_event, NULL);
    if (rc != 0) { ESP_LOGE(TAG, "adv_start rc=%d", rc); return; }
    ESP_LOGI(TAG, "advertising as \"%s\"", s_device_name);
}

/* ----- GAP event dispatch ---------------------------------------- */

static int
nus_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
#if defined(BLE_GAP_EVENT_LINK_ESTAB)
    case BLE_GAP_EVENT_LINK_ESTAB:
#endif
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            /* Guard against the LINK_ESTAB + CONNECT double-fire on
             * newer NimBLE — security_initiate is idempotent but we
             * don't need to log twice. */
            if (s_conn_handle == event->connect.conn_handle) return 0;
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected, handle=%d — initiating security",
                     s_conn_handle);
            /* Proactively start encryption so pairing happens as part
             * of the connection sequence, not reactively when the
             * central hits an encrypted characteristic. If we're
             * already bonded NimBLE silently re-encrypts with the
             * stored LTK. The app is notified of "connected" only
             * after BLE_GAP_EVENT_ENC_CHANGE fires below. */
            int rc = ble_gap_security_initiate(s_conn_handle);
            if (rc != 0 && rc != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "security_initiate rc=%d — disconnecting", rc);
                ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
        } else {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            nus_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected reason=%d",
                 event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_tx_subscribed = false;
        if (s_on_passkey)    s_on_passkey(0);
        if (s_on_encrypted)  s_on_encrypted(false);
        if (s_on_conn)       s_on_conn(false);
        nus_advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        nus_advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_val_handle) {
            s_tx_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "TX notify subscribed=%d", s_tx_subscribed);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU update conn=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc desc;
        bool enc = false, authen = false, bonded = false;
        if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
            enc    = desc.sec_state.encrypted;
            authen = desc.sec_state.authenticated;
            bonded = desc.sec_state.bonded;
        }
        ESP_LOGI(TAG, "encryption change status=%d enc=%d authen=%d bonded=%d",
                 event->enc_change.status, enc, authen, bonded);
        if (s_on_passkey)   s_on_passkey(0);   /* hide passkey UI */
        if (event->enc_change.status != 0 || !enc) {
            /* Pairing failed / was rejected. Drop the connection so
             * we don't sit there with a plaintext link. */
            ESP_LOGW(TAG, "encryption failed; disconnecting");
            ble_gap_terminate(event->enc_change.conn_handle,
                              BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        if (s_on_encrypted) s_on_encrypted(true);
        /* The app learns about "connected" only once the link is
         * encrypted — up to this point the face UI has been in its
         * pairing state (passkey panel over SLEEP). */
        if (s_on_conn) s_on_conn(true);
        return 0;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            /* Generate a uniformly random 6-digit passkey and show it
             * on the aperture. Central echoes it in the OS dialog. */
            uint32_t pk = esp_random() % 1000000;
            ESP_LOGI(TAG, "display passkey %06" PRIu32, pk);
            if (s_on_passkey) s_on_passkey(pk);
            struct ble_sm_io pk_io = {
                .action = BLE_SM_IOACT_DISP,
                .passkey = pk,
            };
            int rc = ble_sm_inject_io(event->passkey.conn_handle, &pk_io);
            if (rc != 0) {
                ESP_LOGE(TAG, "ble_sm_inject_io rc=%d", rc);
            }
        } else {
            ESP_LOGW(TAG, "unhandled passkey action %d",
                     event->passkey.params.action);
        }
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* Central says "we were bonded but I lost my keys". Drop the
         * stored bond and let them pair fresh. */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        ESP_LOGW(TAG, "repeat pairing — accepting fresh pair");
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        return 0;
    }
}

/* ----- Host lifecycle -------------------------------------------- */

static void
nus_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr rc=%d", rc);
        return;
    }
    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "device addr %02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    nus_advertise();
}

static void
nus_on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset reason=%d", reason);
}

static void
ble_host_task(void *param)
{
    ESP_LOGI(TAG, "nimble host task running");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ----- Public API ------------------------------------------------ */

esp_err_t
ble_nus_send(const uint8_t *data, size_t len)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_tx_subscribed) {
        return ESP_ERR_INVALID_STATE;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) return ESP_ERR_NO_MEM;
    int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t
ble_nus_unpair_all(void)
{
    /* Drop the current central first so the bond table is quiescent. */
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    int rc = ble_store_clear();
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_store_clear rc=%d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "all bonds erased");
    return ESP_OK;
}

esp_err_t
ble_nus_start(const ble_nus_cfg_t *cfg)
{
    if (!cfg || !cfg->device_name) return ESP_ERR_INVALID_ARG;

    s_on_rx        = cfg->on_rx;
    s_on_conn      = cfg->on_conn;
    s_on_passkey   = cfg->on_passkey;
    s_on_encrypted = cfg->on_encrypted;
    strncpy(s_device_name, cfg->device_name, sizeof(s_device_name) - 1);
    s_device_name[sizeof(s_device_name) - 1] = '\0';

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Bring up the esp-hosted link to the C6. The stock Waveshare C6
     * firmware reports version 0.0.0 and does not respond to the newer
     * RPC gate calls (bt_controller_init/enable), so we skip those —
     * NimBLE's own HCI RESET at port_init is enough. See
     * memory/project_c6_rpc.md. */
    ESP_ERROR_CHECK(esp_hosted_connect_to_slave());
    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.reset_cb         = nus_on_reset;
    ble_hs_cfg.sync_cb          = nus_on_sync;
    ble_hs_cfg.store_status_cb  = ble_store_util_status_rr;

    /* LE Secure Connections with MITM (passkey). DisplayOnly IO →
     * the device shows a 6-digit code and the central has to type it
     * back in its OS pairing dialog. Distribute encryption + identity
     * keys so reconnects are silent. */
    ble_hs_cfg.sm_io_cap        = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_sc            = 1;
    ble_hs_cfg.sm_bonding       = 1;
    ble_hs_cfg.sm_mitm          = 1;
    ble_hs_cfg.sm_our_key_dist  = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(nus_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "gatts_count_cfg rc=%d", rc); return ESP_FAIL; }
    rc = ble_gatts_add_svcs(nus_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "gatts_add_svcs rc=%d", rc); return ESP_FAIL; }

    rc = ble_svc_gap_device_name_set(s_device_name);
    if (rc != 0) { ESP_LOGE(TAG, "name_set rc=%d", rc); return ESP_FAIL; }

    ble_store_config_init();
    nimble_port_freertos_init(ble_host_task);
    return ESP_OK;
}
