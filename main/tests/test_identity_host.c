#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "native_identity.h"
#include "tater_config.h"
#include "nvs.h"

static unsigned char committed[128], pending[128];
static bool has_committed, has_pending, fail_write, fail_commit;
static int string_writes;

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{ (void)name; (void)mode; *handle = 1; return ESP_OK; }
void nvs_close(nvs_handle_t handle) { (void)handle; has_pending = false; }
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *size)
{
    (void)handle;
    const char *value = strcmp(key, "ssid") == 0 ? "wifi-kept"
        : strcmp(key, "pass") == 0 ? "password-kept"
        : strcmp(key, "server") == 0 ? "http://ha.test"
        : strcmp(key, "token") == 0 ? "token-kept"
        : strcmp(key, "name") == 0 ? "Office Echo" : "Office";
    assert(strlen(value) + 1 <= *size);
    strcpy(out, value); return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{ (void)handle; (void)key; (void)value; ++string_writes; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size)
{
    (void)handle; assert(strcmp(key, "identity") == 0);
    if (!has_committed) return ESP_ERR_NVS_NOT_FOUND;
    assert(*size == sizeof(committed)); memcpy(out, committed, *size); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size)
{
    (void)handle; assert(strcmp(key, "identity") == 0); assert(size == sizeof(pending));
    if (fail_write) return ESP_FAIL;
    memcpy(pending, value, size); has_pending = true; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    if (fail_commit) return ESP_FAIL;
    if (has_pending) { memcpy(committed, pending, sizeof(committed)); has_committed = true; }
    return ESP_OK;
}
esp_err_t nvs_erase_all(nvs_handle_t handle) { (void)handle; has_committed = false; return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) { (void)handle; (void)key; return ESP_OK; }

int main(void)
{
    char value[64];
    assert(native_identity_text(" Kitchen Echo ", value, sizeof(value), true));
    assert(strcmp(value, "Kitchen Echo") == 0);
    assert(native_identity_text("", value, sizeof(value), false));
    assert(!native_identity_text("", value, sizeof(value), true));
    assert(!native_identity_text("bad\nname", value, sizeof(value), true));
    char long_name[65]; memset(long_name, 'x', 64); long_name[64] = 0;
    assert(!native_identity_text(long_name, value, sizeof(value), true));
    long_name[63] = 0;
    assert(native_identity_text(long_name, value, sizeof(value), true));

    tater_config_t config;
    assert(tater_config_load(&config) == ESP_OK);
    assert(strcmp(config.device_name, "Office Echo") == 0);
    assert(tater_config_save_identity("Kitchen Echo", "") == ESP_OK);
    assert(tater_config_load(&config) == ESP_OK);
    assert(strcmp(config.device_name, "Kitchen Echo") == 0 && config.room[0] == 0);
    assert(strcmp(config.token, "token-kept") == 0);
    assert(strcmp(config.wifi_ssid, "wifi-kept") == 0);
    assert(strcmp(config.server_url, "http://ha.test") == 0);
    assert(string_writes == 0);

    fail_write = true;
    assert(tater_config_save_identity("Bedroom Echo", "Bedroom") == ESP_FAIL);
    fail_write = false; fail_commit = true;
    assert(tater_config_save_identity("Bedroom Echo", "Bedroom") == ESP_FAIL);
    fail_commit = false;
    assert(tater_config_load(&config) == ESP_OK);
    assert(strcmp(config.device_name, "Kitchen Echo") == 0 && config.room[0] == 0);
    assert(tater_config_save_identity("", "Office") == ESP_ERR_INVALID_ARG);
    /* An explicit name must not be mistaken for an old provisioning default. */
    assert(tater_config_save_identity("Tater Voice PE", "Studio") == ESP_OK);
    assert(tater_config_load(&config) == ESP_OK);
    assert(strcmp(config.device_name, "Tater Voice PE") == 0);
    puts("Native identity persistence and validation passed");
    return 0;
}
