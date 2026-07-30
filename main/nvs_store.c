#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "nvs_store.h"

#define TAG "nvs"

#define NVS_READ(ns, expr) ({ \
    nvs_handle_t _h; \
    esp_err_t _e = nvs_open(ns, NVS_READONLY, &_h); \
    if (_e == ESP_OK) { _e = (expr); nvs_close(_h); } \
    if (_e != ESP_OK) ESP_LOGW(TAG, "read %s failed: %s", ns, esp_err_to_name(_e)); \
    _e; \
})

#define NVS_WRITE(ns, expr) ({ \
    nvs_handle_t _h; \
    esp_err_t _e = nvs_open(ns, NVS_READWRITE, &_h); \
    if (_e == ESP_OK) { \
        _e = (expr); \
        if (_e == ESP_OK) _e = nvs_commit(_h); \
        nvs_close(_h); \
    } \
    if (_e != ESP_OK) ESP_LOGW(TAG, "write %s failed: %s", ns, esp_err_to_name(_e)); \
    _e; \
})

esp_err_t nvs_store_get_u8(const char *ns, const char *key, uint8_t *out) {
    return NVS_READ(ns, nvs_get_u8(_h, key, out));
}
esp_err_t nvs_store_set_u8(const char *ns, const char *key, uint8_t val) {
    return NVS_WRITE(ns, nvs_set_u8(_h, key, val));
}
esp_err_t nvs_store_get_u32(const char *ns, const char *key, uint32_t *out) {
    return NVS_READ(ns, nvs_get_u32(_h, key, out));
}
esp_err_t nvs_store_set_u32(const char *ns, const char *key, uint32_t val) {
    return NVS_WRITE(ns, nvs_set_u32(_h, key, val));
}
esp_err_t nvs_store_get_blob(const char *ns, const char *key, void *out, size_t *sz) {
    return NVS_READ(ns, nvs_get_blob(_h, key, out, sz));
}
esp_err_t nvs_store_set_blob(const char *ns, const char *key, const void *val, size_t sz) {
    return NVS_WRITE(ns, nvs_set_blob(_h, key, val, sz));
}
esp_err_t nvs_store_erase(const char *ns, const char *key) {
    return NVS_WRITE(ns, nvs_erase_key(_h, key));
}
esp_err_t nvs_store_erase_all(const char *ns) {
    return NVS_WRITE(ns, nvs_erase_all(_h));
}
