#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define TATER_CFG_WIFI_SSID_LEN 33
#define TATER_CFG_WIFI_PASSWORD_LEN 65
#define TATER_CFG_SERVER_URL_LEN 160
#define TATER_CFG_TOKEN_LEN 128
#define TATER_CFG_DEVICE_NAME_LEN 64
#define TATER_CFG_ROOM_LEN 64

typedef struct {
    char wifi_ssid[TATER_CFG_WIFI_SSID_LEN];
    char wifi_password[TATER_CFG_WIFI_PASSWORD_LEN];
    char server_url[TATER_CFG_SERVER_URL_LEN];
    char token[TATER_CFG_TOKEN_LEN];
    char device_name[TATER_CFG_DEVICE_NAME_LEN];
    char room[TATER_CFG_ROOM_LEN];
    bool provisioned;
} tater_config_t;

void tater_config_defaults(tater_config_t *config);
esp_err_t tater_config_load(tater_config_t *config);
esp_err_t tater_config_save(const tater_config_t *config);
esp_err_t tater_config_save_token(const char *token);
esp_err_t tater_config_clear(void);
esp_err_t tater_config_forget_wifi(void);
bool tater_config_has_wifi(const tater_config_t *config);
