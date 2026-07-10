#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t nvs_store_get_u8(const char *ns, const char *key, uint8_t *out);
esp_err_t nvs_store_set_u8(const char *ns, const char *key, uint8_t val);
esp_err_t nvs_store_get_u32(const char *ns, const char *key, uint32_t *out);
esp_err_t nvs_store_set_u32(const char *ns, const char *key, uint32_t val);
esp_err_t nvs_store_get_blob(const char *ns, const char *key, void *out, size_t *sz);
esp_err_t nvs_store_set_blob(const char *ns, const char *key, const void *val, size_t sz);
esp_err_t nvs_store_erase(const char *ns, const char *key);
esp_err_t nvs_store_erase_all(const char *ns);
