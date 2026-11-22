"""Run the production NVS identity code against a fault-injecting host store."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile

MAIN = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="tater-identity-") as directory:
    root = Path(directory)
    for filename in ("tater_config.c", "tater_config.h", "native_identity.h"):
        shutil.copy2(MAIN / filename, root / filename)
    (root / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_INVALID_ARG -2\n#define ESP_ERR_NVS_NOT_FOUND -3\n")
    (root / "esp_check.h").write_text('#pragma once\n#define ESP_RETURN_ON_FALSE(test, err, tag, msg) do { (void)(tag); if (!(test)) return (err); } while (0)\n#define ESP_RETURN_ON_ERROR(expr, tag, msg) do { (void)(tag); esp_err_t e = (expr); if (e != ESP_OK) return e; } while (0)\n#define ESP_ERROR_CHECK_WITHOUT_ABORT(expr) ((void)(expr))\n')
    (root / "board.h").write_text('#pragma once\n#define TATER_DEFAULT_DEVICE_NAME "Tater SAT1"\n#define TATER_BOARD_VOICE_PE 0\n#define CONFIG_TATER_WIFI_SSID ""\n#define CONFIG_TATER_WIFI_PASSWORD ""\n#define CONFIG_TATER_SERVER_URL ""\n#define CONFIG_TATER_SATELLITE_TOKEN ""\n#define CONFIG_TATER_ROOM ""\n')
    (root / "nvs.h").write_text('''#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef int nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t nvs_erase_all(nvs_handle_t);
esp_err_t nvs_erase_key(nvs_handle_t, const char *);
''')
    executable = root / "test-identity"
    (root / "compat.h").write_text('#include <string.h>\nstatic inline size_t test_strlcpy(char *out, const char *in, size_t size) { size_t n = strlen(in); if (size) { size_t copy = n < size - 1 ? n : size - 1; memcpy(out, in, copy); out[copy] = 0; } return n; }\n#undef strlcpy\n#define strlcpy test_strlcpy\n')
    for usb_enabled in (0, 1):
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", f"-DCONFIG_TATER_USB_ETHERNET={usb_enabled}", "-include", str(root / "compat.h"), "-I", str(root), str(root / "tater_config.c"), str(MAIN / "tests/test_identity_host.c"), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
