idf_component_register(
    SRCS "main.c" "face.c" "ble_nus.c" "cdb_protocol.c" "audio.c" "asset_push.c"
    INCLUDE_DIRS "."
    REQUIRES bt esp_hosted nvs_flash json spiffs mbedtls
)
