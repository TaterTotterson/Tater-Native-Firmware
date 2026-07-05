#include "tater_config.h"

#include <string.h>

#include "esp_check.h"
#include "nvs.h"

static const char *TAG = "tater_config";
static const char *NVS_NAMESPACE = "tater";

static void safe_copy(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    strlcpy(dst, src ? src : "", dst_len);
}

void tater_config_defaults(tater_config_t *config)
{
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    safe_copy(config->wifi_ssid, sizeof(config->wifi_ssid), CONFIG_TATER_WIFI_SSID);
    safe_copy(config->wifi_password, sizeof(config->wifi_password), CONFIG_TATER_WIFI_PASSWORD);
    safe_copy(config->server_url, sizeof(config->server_url), CONFIG_TATER_SERVER_URL);
    safe_copy(config->token, sizeof(config->token), CONFIG_TATER_SATELLITE_TOKEN);
    safe_copy(config->device_name, sizeof(config->device_name), CONFIG_TATER_DEVICE_NAME);
    safe_copy(config->room, sizeof(config->room), CONFIG_TATER_ROOM);
    config->provisioned = false;
}

static void load_string(nvs_handle_t nvs, const char *key, char *dst, size_t dst_len)
{
    size_t len = dst_len;
    esp_err_t err = nvs_get_str(nvs, key, dst, &len);
    if (err != ESP_OK && dst_len > 0) {
        dst[0] = '\0';
    }
}

esp_err_t tater_config_load(tater_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is null");
    tater_config_defaults(config);

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs open failed");

    load_string(nvs, "ssid", config->wifi_ssid, sizeof(config->wifi_ssid));
    load_string(nvs, "pass", config->wifi_password, sizeof(config->wifi_password));
    load_string(nvs, "server", config->server_url, sizeof(config->server_url));
    load_string(nvs, "token", config->token, sizeof(config->token));
    load_string(nvs, "name", config->device_name, sizeof(config->device_name));
    load_string(nvs, "room", config->room, sizeof(config->room));
    nvs_close(nvs);

    config->provisioned = strlen(config->wifi_ssid) > 0;
    if (strlen(config->server_url) == 0) {
        safe_copy(config->server_url, sizeof(config->server_url), CONFIG_TATER_SERVER_URL);
    }
    if (strlen(config->device_name) == 0) {
        safe_copy(config->device_name, sizeof(config->device_name), CONFIG_TATER_DEVICE_NAME);
    }
    return ESP_OK;
}

esp_err_t tater_config_save(const tater_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is null");
    ESP_RETURN_ON_FALSE(strlen(config->wifi_ssid) > 0, ESP_ERR_INVALID_ARG, TAG, "ssid is empty");
    ESP_RETURN_ON_FALSE(strlen(config->server_url) > 0, ESP_ERR_INVALID_ARG, TAG, "server url is empty");

    nvs_handle_t nvs = 0;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs open failed");
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(nvs, "ssid", config->wifi_ssid));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(nvs, "pass", config->wifi_password));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(nvs, "server", config->server_url));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(nvs, "token", config->token));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(nvs, "name", config->device_name));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(nvs, "room", config->room));
    esp_err_t err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t tater_config_save_token(const char *token)
{
    ESP_RETURN_ON_FALSE(token, ESP_ERR_INVALID_ARG, TAG, "token is null");
    nvs_handle_t nvs = 0;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs open failed");
    esp_err_t err = nvs_set_str(nvs, "token", token);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t tater_config_clear(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs open failed");
    err = nvs_erase_all(nvs);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t tater_config_forget_wifi(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs open failed");
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_erase_key(nvs, "ssid"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_erase_key(nvs, "pass"));
    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

bool tater_config_has_wifi(const tater_config_t *config)
{
    return config && strlen(config->wifi_ssid) > 0;
}
