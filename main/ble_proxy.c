#include "ble_proxy.h"
#include "ble_scanner.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"

/* Discovery trees live in PSRAM. Keeping hundreds of small cJSON allocations
 * in internal RAM would compete directly with Wi-Fi and the BLE controller. */
static volatile bool s_json_failed;
static cJSON *proxy_json(int type)
{
    cJSON *item = heap_caps_calloc(1, sizeof(cJSON), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (item) item->type = type;
    else s_json_failed = true;
    return item;
}
static cJSON *proxy_object(void) { return proxy_json(cJSON_Object); }
static cJSON *proxy_array(void) { return proxy_json(cJSON_Array); }
static cJSON *proxy_string(const char *text)
{
    cJSON *item = proxy_json(cJSON_String);
    if (item) {
        size_t length = strlen(text) + 1;
        item->valuestring = heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!item->valuestring) { s_json_failed = true; free(item); return NULL; }
        memcpy(item->valuestring, text, length);
    }
    return item;
}
static cJSON *proxy_add(cJSON *object, const char *key, cJSON *value)
{
    if (!value) { s_json_failed = true; return NULL; }
    if (!cJSON_AddItemToObjectCS(object, key, value)) { s_json_failed = true; cJSON_Delete(value); return NULL; }
    return value;
}
static cJSON *proxy_add_string(cJSON *object, const char *key, const char *text) { return proxy_add(object, key, proxy_string(text)); }
static cJSON *proxy_add_array(cJSON *object, const char *key) { return proxy_add(object, key, proxy_array()); }
static cJSON *proxy_add_number(cJSON *object, const char *key, double number)
{
    cJSON *value = proxy_json(cJSON_Number);
    if (value) { value->valuedouble = number; value->valueint = number > INT32_MAX ? INT32_MAX : (int)number; }
    return proxy_add(object, key, value);
}
static cJSON *proxy_add_bool(cJSON *object, const char *key, bool value) { return proxy_add(object, key, proxy_json(value ? cJSON_True : cJSON_False)); }
#define cJSON_CreateObject proxy_object
#define cJSON_CreateString proxy_string
#define cJSON_AddStringToObject proxy_add_string
#define cJSON_AddArrayToObject proxy_add_array
#define cJSON_AddNumberToObject proxy_add_number
#define cJSON_AddBoolToObject proxy_add_bool

#define VALUE_MAX 512
#define REQUEST_MAX 8
#define EVENT_MAX 16
#define WAIT_MS 22000
#define MAX_SERVICES 16
#define MAX_CHARACTERISTICS 64
#define MAX_DESCRIPTORS 96

typedef struct {
    char message_id[160];
    char action[16];
    uint32_t req;
    uint32_t epoch;
    char address[18];
    ble_addr_t peer;
    uint16_t handle;
    uint16_t length;
    uint8_t value[VALUE_MAX];
    bool response;
    char pin[7];
    uint32_t seconds;
} request_t;

typedef struct {
    bool notify;
    bool indication;
    char address[18];
    uint16_t handle;
    uint16_t length;
    uint8_t value[VALUE_MAX];
} event_t;

static tater_ble_proxy_send_t s_send;
static QueueHandle_t s_requests;
static QueueHandle_t s_events;
static TaskHandle_t s_worker;
static StaticSemaphore_t s_op_lock_storage;
static StaticSemaphore_t s_done_storage;
static SemaphoreHandle_t s_op_lock;
static SemaphoreHandle_t s_done;
static portMUX_TYPE s_init_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_starting;
static volatile uint32_t s_epoch;
static volatile uint16_t s_connection = BLE_HS_CONN_HANDLE_NONE;
static char s_address[18];
static ble_addr_t s_peer;
static uint32_t s_generation;
static uint32_t s_operation_epoch;
static bool s_waiting;
static bool s_connecting;
static bool s_pairing;
static bool s_security_waiting;
static bool s_read_long;
static char s_pin[7];
static int s_error;
static cJSON *s_target;
static uint8_t s_read[VALUE_MAX];
static uint16_t s_read_len;
static size_t s_service_count, s_characteristic_count, s_descriptor_count;
static volatile bool s_reset;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void address_text(const ble_addr_t *address, char *out)
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", address->val[5], address->val[4], address->val[3], address->val[2], address->val[1], address->val[0]);
}
static bool parse_address(const char *value, ble_addr_t *out)
{
    if (!value || strlen(value) != 17) return false;
    for (size_t i = 0; i < 17; i++) {
        if (i % 3 == 2) { if (value[i] != ':') return false; }
        else if (!isxdigit((unsigned char)value[i])) return false;
    }
    for (size_t i = 0; i < 6; i++) {
        char byte[3] = {value[i * 3], value[i * 3 + 1], 0};
        out->val[5 - i] = (uint8_t)strtoul(byte, NULL, 16);
    }
    return true;
}
static cJSON *base_result(const request_t *request)
{
    cJSON *value = cJSON_CreateObject();
    cJSON_AddStringToObject(value, "reply_to", request->message_id);
    cJSON_AddStringToObject(value, "t", "result");
    cJSON_AddNumberToObject(value, "req", request->req);
    return value;
}
static void reply_error(const request_t *request, const char *error)
{
    cJSON *value = base_result(request);
    cJSON_AddBoolToObject(value, "ok", false);
    cJSON_AddStringToObject(value, "error", error);
    s_send("ble.gatt.result", value);
}
static void send_slots(void)
{
    tater_ble_scanner_stats_t stats = {0};
    tater_ble_scanner_get_stats(&stats);
    cJSON *value = cJSON_CreateObject();
    cJSON_AddStringToObject(value, "t", "slots");
    cJSON_AddNumberToObject(value, "limit", 1);
    cJSON_AddNumberToObject(value, "free", s_connection == BLE_HS_CONN_HANDLE_NONE ? 1 : 0);
    cJSON_AddBoolToObject(value, "enabled", stats.initialized);
    cJSON_AddBoolToObject(value, "scanning", stats.scanning);
    cJSON *addresses = cJSON_AddArrayToObject(value, "addrs");
    if (s_connection != BLE_HS_CONN_HANDLE_NONE) cJSON_AddItemToArray(addresses, cJSON_CreateString(s_address));
    s_send("ble.gatt.event", value);
}
static void add_encoded(cJSON *value, const uint8_t *bytes, size_t length)
{
    unsigned char encoded[(VALUE_MAX + 2) / 3 * 4 + 1];
    size_t actual = 0;
    if (mbedtls_base64_encode(encoded, sizeof(encoded), &actual, bytes, length) == 0) {
        encoded[actual] = 0;
        cJSON_AddStringToObject(value, "value", (char *)encoded);
    }
}
static uint32_t operation_begin(cJSON *target)
{
    xSemaphoreTake(s_op_lock, portMAX_DELAY);
    while (xSemaphoreTake(s_done, 0) == pdTRUE) {}
    ++s_generation;
    if (!s_generation) ++s_generation;
    s_waiting = true;
    s_operation_epoch = s_epoch;
    s_error = 0;
    s_target = target;
    s_read_len = 0;
    s_read_long = false;
    s_security_waiting = false;
    uint32_t generation = s_generation;
    xSemaphoreGive(s_op_lock);
    return generation;
}
static void operation_complete(int error)
{
    if (s_waiting) {
        s_error = error;
        s_waiting = false;
        xSemaphoreGive(s_done);
    }
}
static int operation_wait(int started)
{
    if (!started && xSemaphoreTake(s_done, pdMS_TO_TICKS(WAIT_MS)) != pdTRUE) started = BLE_HS_ETIMEOUT;
    xSemaphoreTake(s_op_lock, portMAX_DELAY);
    int error = started ? started : s_error;
    s_waiting = false;
    s_target = NULL;
    s_pairing = false;
    s_security_waiting = false;
    memset(s_pin, 0, sizeof(s_pin));
    ++s_generation;  /* Retire late callbacks before buffers are reused. */
    xSemaphoreGive(s_op_lock);
    if (error == BLE_HS_ETIMEOUT) {
        ble_gap_conn_cancel();
        if (s_connection != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_connection, BLE_ERR_REM_USER_CONN_TERM);
    }
    return error;
}
static bool operation_current(void *arg)
{
    return s_waiting && (uint32_t)(uintptr_t)arg == s_generation && s_operation_epoch == s_epoch;
}
static int service_cb(uint16_t connection, const struct ble_gatt_error *error, const struct ble_gatt_svc *service, void *arg)
{
    (void)connection;
    xSemaphoreTake(s_op_lock, portMAX_DELAY);
    if (operation_current(arg)) {
        if (error->status) operation_complete(error->status == BLE_HS_EDONE ? 0 : error->status);
        else if (++s_service_count > MAX_SERVICES) operation_complete(BLE_HS_ENOMEM);
        else {
            char uuid[BLE_UUID_STR_LEN]; ble_uuid_to_str(&service->uuid.u, uuid);
            cJSON *row = cJSON_CreateObject();
            cJSON_AddStringToObject(row, "uuid", uuid);
            cJSON_AddNumberToObject(row, "start", service->start_handle);
            cJSON_AddNumberToObject(row, "end", service->end_handle);
            cJSON_AddArrayToObject(row, "chars");
            cJSON_AddItemToArray(s_target, row);
            if (s_json_failed) operation_complete(BLE_HS_ENOMEM);
        }
    }
    xSemaphoreGive(s_op_lock);
    return 0;
}
static int characteristic_cb(uint16_t connection, const struct ble_gatt_error *error, const struct ble_gatt_chr *characteristic, void *arg)
{
    (void)connection;
    xSemaphoreTake(s_op_lock, portMAX_DELAY);
    if (operation_current(arg)) {
        if (error->status) operation_complete(error->status == BLE_HS_EDONE ? 0 : error->status);
        else if (++s_characteristic_count > MAX_CHARACTERISTICS) operation_complete(BLE_HS_ENOMEM);
        else {
            char uuid[BLE_UUID_STR_LEN]; ble_uuid_to_str(&characteristic->uuid.u, uuid);
            cJSON *row = cJSON_CreateObject();
            cJSON_AddStringToObject(row, "uuid", uuid);
            cJSON_AddNumberToObject(row, "handle", characteristic->def_handle);
            cJSON_AddNumberToObject(row, "value_handle", characteristic->val_handle);
            cJSON_AddNumberToObject(row, "props", characteristic->properties);
            cJSON_AddArrayToObject(row, "descs");
            cJSON_AddItemToArray(s_target, row);
            if (s_json_failed) operation_complete(BLE_HS_ENOMEM);
        }
    }
    xSemaphoreGive(s_op_lock);
    return 0;
}
static int descriptor_cb(uint16_t connection, const struct ble_gatt_error *error, uint16_t characteristic, const struct ble_gatt_dsc *descriptor, void *arg)
{
    (void)connection; (void)characteristic;
    xSemaphoreTake(s_op_lock, portMAX_DELAY);
    if (operation_current(arg)) {
        if (error->status) operation_complete(error->status == BLE_HS_EDONE ? 0 : error->status);
        else if (++s_descriptor_count > MAX_DESCRIPTORS) operation_complete(BLE_HS_ENOMEM);
        else {
            char uuid[BLE_UUID_STR_LEN]; ble_uuid_to_str(&descriptor->uuid.u, uuid);
            cJSON *row = cJSON_CreateObject();
            cJSON_AddStringToObject(row, "uuid", uuid);
            cJSON_AddNumberToObject(row, "handle", descriptor->handle);
            cJSON_AddItemToArray(s_target, row);
            if (s_json_failed) operation_complete(BLE_HS_ENOMEM);
        }
    }
    xSemaphoreGive(s_op_lock);
    return 0;
}
static int attribute_cb(uint16_t connection, const struct ble_gatt_error *error, struct ble_gatt_attr *attribute, void *arg)
{
    (void)connection;
    xSemaphoreTake(s_op_lock, portMAX_DELAY);
    if (operation_current(arg)) {
        if (!error->status && attribute && attribute->om) {
            uint16_t length = OS_MBUF_PKTLEN(attribute->om);
            if (length > VALUE_MAX) operation_complete(BLE_HS_EMSGSIZE);
            else {
                if ((size_t)s_read_len + length > VALUE_MAX) operation_complete(BLE_HS_EMSGSIZE);
                else {
                    os_mbuf_copydata(attribute->om, 0, length, s_read + s_read_len);
                    s_read_len += length;
                    if (!s_read_long) operation_complete(0);
                }
            }
        } else operation_complete(error->status == BLE_HS_EDONE ? 0 : error->status);
    }
    xSemaphoreGive(s_op_lock);
    return 0;
}
static int mtu_cb(uint16_t connection, const struct ble_gatt_error *error, uint16_t mtu, void *arg)
{
    (void)connection; (void)mtu;
    xSemaphoreTake(s_op_lock, portMAX_DELAY);
    if (operation_current(arg)) operation_complete(error->status);
    xSemaphoreGive(s_op_lock);
    return 0;
}
static int connection_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    xSemaphoreTake(s_op_lock, portMAX_DELAY);
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        if (!s_connecting || s_operation_epoch != s_epoch) {
            if (!event->connect.status) ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        } else {
            s_connecting = false;
            if (!event->connect.status) s_connection = event->connect.conn_handle;
            operation_complete(event->connect.status);
        }
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        if (event->disconnect.conn.conn_handle == s_connection) {
            s_connection = BLE_HS_CONN_HANDLE_NONE;
            event_t value = {0};
            strlcpy(value.address, s_address, sizeof(value.address));
            xQueueSend(s_events, &value, 0);
            operation_complete(BLE_HS_ENOTCONN);
        }
    } else if (event->type == BLE_GAP_EVENT_REPEAT_PAIRING) {
        int response = BLE_GAP_REPEAT_PAIRING_IGNORE;
        struct ble_gap_conn_desc descriptor;
        if (s_pairing && event->repeat_pairing.conn_handle == s_connection
            && ble_gap_conn_find(s_connection, &descriptor) == 0
            && ble_store_util_delete_peer(&descriptor.peer_id_addr) == 0) response = BLE_GAP_REPEAT_PAIRING_RETRY;
        xSemaphoreGive(s_op_lock);
        return response;
    } else if (event->type == BLE_GAP_EVENT_ENC_CHANGE) {
        if (event->enc_change.conn_handle == s_connection && s_waiting && s_security_waiting) operation_complete(event->enc_change.status);
    } else if (event->type == BLE_GAP_EVENT_PASSKEY_ACTION) {
        if (s_pairing && s_operation_epoch == s_epoch && event->passkey.conn_handle == s_connection) {
            struct ble_sm_io io = {0};
            io.action = event->passkey.params.action;
            io.passkey = (uint32_t)strtoul(s_pin, NULL, 10);
            if (io.action == BLE_SM_IOACT_INPUT) ble_sm_inject_io(s_connection, &io);
            else operation_complete(BLE_HS_EAUTHEN);
        } else ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_AUTH_FAIL);
    } else if (event->type == BLE_GAP_EVENT_NOTIFY_RX && event->notify_rx.conn_handle == s_connection) {
        event_t value = {.notify = true, .indication = event->notify_rx.indication, .handle = event->notify_rx.attr_handle};
        value.length = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (value.length <= VALUE_MAX) {
            strlcpy(value.address, s_address, sizeof(value.address));
            os_mbuf_copydata(event->notify_rx.om, 0, value.length, value.value);
            xQueueSend(s_events, &value, 0);
        }
    }
    xSemaphoreGive(s_op_lock);
    return 0;
}
void tater_ble_proxy_host_reset(void)
{
    xSemaphoreTake(s_op_lock, portMAX_DELAY);
    ++s_epoch;
    s_reset = true;
    if (s_connection != BLE_HS_CONN_HANDLE_NONE && s_events) {
        event_t value = {0};
        strlcpy(value.address, s_address, sizeof(value.address));
        xQueueSend(s_events, &value, 0);
    }
    s_connection = BLE_HS_CONN_HANDLE_NONE;
    s_connecting = false;
    operation_complete(BLE_HS_ECONTROLLER);
    xSemaphoreGive(s_op_lock);
}
extern void ble_store_config_init(void);
void tater_ble_proxy_host_config(void)
{
    ble_att_set_preferred_mtu(247);
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();
}
static int ensure_stack(void)
{
    if (tater_ble_stack_start() != ESP_OK) return BLE_HS_ENOMEM;
    uint32_t started = now_ms();
    while ((uint32_t)(now_ms() - started) < 6000) {
        tater_ble_scanner_stats_t stats = {0}; tater_ble_scanner_get_stats(&stats);
        if (stats.synced) return 0;
        if (s_reset) return BLE_HS_ENOTCONN;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return BLE_HS_ETIMEOUT;
}
static bool bonded_peer(const ble_addr_t *address)
{
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int count = 0;
    if (ble_store_util_bonded_peers(peers, &count, CONFIG_BT_NIMBLE_MAX_BONDS) != 0) return false;
    for (int i = 0; i < count; i++) if (peers[i].type == address->type && !memcmp(peers[i].val, address->val, 6)) return true;
    return false;
}
static int discover_services(cJSON *services)
{
    s_service_count = s_characteristic_count = s_descriptor_count = 0;
    uint32_t op = operation_begin(services);
    int error = operation_wait(ble_gattc_disc_all_svcs(s_connection, service_cb, (void *)(uintptr_t)op));
    cJSON *service;
    cJSON_ArrayForEach(service, services) {
        if (error) break;
        uint16_t start = cJSON_GetObjectItem(service, "start")->valueint;
        uint16_t end = cJSON_GetObjectItem(service, "end")->valueint;
        cJSON *chars = cJSON_GetObjectItem(service, "chars");
        op = operation_begin(chars);
        error = operation_wait(ble_gattc_disc_all_chrs(s_connection, start, end, characteristic_cb, (void *)(uintptr_t)op));
        cJSON *characteristic;
        cJSON_ArrayForEach(characteristic, chars) {
            if (error) break;
            uint16_t value = cJSON_GetObjectItem(characteristic, "value_handle")->valueint;
            uint16_t last = characteristic->next ? cJSON_GetObjectItem(characteristic->next, "handle")->valueint - 1 : end;
            if (last <= value) continue;
            op = operation_begin(cJSON_GetObjectItem(characteristic, "descs"));
            error = operation_wait(ble_gattc_disc_all_dscs(s_connection, value, last, descriptor_cb, (void *)(uintptr_t)op));
        }
    }
    return error;
}
static void run_request(const request_t *request)
{
    if (request->epoch != s_epoch) { reply_error(request, "disconnected"); return; }
    s_reset = false;
    s_json_failed = false;
    cJSON *result = base_result(request);
    int error = 0;
    const char *failure = NULL;
    bool has_address = request->address[0];
    bool same_peer = has_address && s_connection != BLE_HS_CONN_HANDLE_NONE && !strcmp(request->address, s_address);
    if (!strcmp(request->action, "slots")) {
        cJSON_AddNumberToObject(result, "limit", 1);
        cJSON_AddNumberToObject(result, "free", s_connection == BLE_HS_CONN_HANDLE_NONE ? 1 : 0);
        cJSON *addresses = cJSON_AddArrayToObject(result, "addrs");
        if (s_connection != BLE_HS_CONN_HANDLE_NONE) cJSON_AddItemToArray(addresses, cJSON_CreateString(s_address));
    } else if (!strcmp(request->action, "scan")) {
        error = ensure_stack();
        if (!error) tater_ble_scanner_discover(request->seconds * 1000);
    } else if (!strcmp(request->action, "stop_scan")) tater_ble_scanner_discover(0);
    else if (!strcmp(request->action, "connect")) {
        if (same_peer) failure = "already_connected";
        else if (s_connection != BLE_HS_CONN_HANDLE_NONE) failure = "no_slots";
        else if (!(error = ensure_stack())) {
            tater_ble_scanner_hold(true);
            ble_gap_disc_cancel();
            s_peer = request->peer;
            strlcpy(s_address, request->address, sizeof(s_address));
            uint8_t own = 0; error = ble_hs_id_infer_auto(0, &own);
            uint32_t op = operation_begin(NULL);
            (void)op;
            s_connecting = true;
            if (!error) error = ble_gap_connect(own, &s_peer, 18000, NULL, connection_event, NULL);
            error = operation_wait(error);
            s_connecting = false;
            if (!error) {
                uint32_t mtu_op = operation_begin(NULL);
                // A peer may retain the default MTU; that is still a valid link.
                (void)operation_wait(ble_gattc_exchange_mtu(s_connection, mtu_cb, (void *)(uintptr_t)mtu_op));
                if (s_connection == BLE_HS_CONN_HANDLE_NONE) error = BLE_HS_ENOTCONN;
            }
            struct ble_gap_conn_desc current;
            if (!error && ble_gap_conn_find(s_connection, &current) == 0 && bonded_peer(&current.peer_id_addr)) {
                operation_begin(NULL);
                s_security_waiting = true;
                error = operation_wait(ble_gap_security_initiate(s_connection));
            }
            tater_ble_scanner_hold(false);
            if (!error) cJSON_AddNumberToObject(result, "mtu", ble_att_mtu(s_connection));
        }
    } else if (!strcmp(request->action, "forget")) {
        error = ensure_stack();
        if (!error) {
            // The wire identifies a bond by MAC. Clients do not need to retain
            // its original address type; use the stored/resolved identity.
            ble_addr_t identity = request->peer;
            struct ble_gap_conn_desc current;
            if (same_peer && ble_gap_conn_find(s_connection, &current) == 0) {
                identity = current.peer_id_addr;
                ble_gap_terminate(s_connection, BLE_ERR_REM_USER_CONN_TERM);
            } else {
                ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
                int count = 0;
                if (ble_store_util_bonded_peers(peers, &count, CONFIG_BT_NIMBLE_MAX_BONDS) == 0) {
                    for (int i = 0; i < count; i++) {
                        if (!memcmp(peers[i].val, identity.val, sizeof(identity.val))) { identity = peers[i]; break; }
                    }
                }
            }
            error = ble_store_util_delete_peer(&identity);
            if (error == BLE_HS_ENOENT) error = 0;
        }
    } else if (!strcmp(request->action, "disconnect")) {
        if (same_peer) error = ble_gap_terminate(s_connection, BLE_ERR_REM_USER_CONN_TERM);
    } else if (!same_peer) failure = "not_connected";
    else if (!strcmp(request->action, "pair")) {
        struct ble_gap_conn_desc descriptor;
        error = ble_gap_conn_find(s_connection, &descriptor);
        if (!error && !(descriptor.sec_state.encrypted && descriptor.sec_state.authenticated && descriptor.sec_state.bonded)) {
            operation_begin(NULL);
            s_pairing = true;
            s_security_waiting = true;
            strlcpy(s_pin, request->pin, sizeof(s_pin));
            error = operation_wait(ble_gap_security_initiate(s_connection));
        }
        if (!error) error = ble_gap_conn_find(s_connection, &descriptor);
        if (!error && !(descriptor.sec_state.encrypted && descriptor.sec_state.authenticated && descriptor.sec_state.bonded)) error = BLE_HS_EAUTHEN;
        if (!error) {
            cJSON_AddBoolToObject(result, "bonded", true);
            cJSON_AddBoolToObject(result, "encrypted", true);
            cJSON_AddBoolToObject(result, "authenticated", true);
        }
    } else if (!strcmp(request->action, "services")) error = discover_services(cJSON_AddArrayToObject(result, "services"));
    else if (!strcmp(request->action, "read")) {
        uint32_t op = operation_begin(NULL);
        s_read_long = true;
        error = operation_wait(ble_gattc_read_long(s_connection, request->handle, 0, attribute_cb, (void *)(uintptr_t)op));
        if (!error) add_encoded(result, s_read, s_read_len);
    } else if (!strcmp(request->action, "write")) {
        if (!request->response) error = ble_gattc_write_no_rsp_flat(s_connection, request->handle, request->value, request->length);
        else {
            uint32_t op = operation_begin(NULL);
            if (request->length > ble_att_mtu(s_connection) - 3) {
                struct os_mbuf *value = ble_hs_mbuf_from_flat(request->value, request->length);
                error = operation_wait(value ? ble_gattc_write_long(s_connection, request->handle, 0, value, attribute_cb, (void *)(uintptr_t)op) : BLE_HS_ENOMEM);
            } else error = operation_wait(ble_gattc_write_flat(s_connection, request->handle, request->value, request->length, attribute_cb, (void *)(uintptr_t)op));
        }
    }
    if (request->epoch != s_epoch) failure = "disconnected";
    if (s_json_failed && !error) error = BLE_HS_ENOMEM;
    cJSON_AddBoolToObject(result, "ok", !error && !failure);
    if (error || failure) {
        cJSON_AddStringToObject(result, "error", failure ? failure : error == BLE_HS_ETIMEOUT ? "timeout" : "failed");
        if (error >= BLE_HS_ERR_ATT_BASE && error < BLE_HS_ERR_ATT_BASE + 256) {
            cJSON_ReplaceItemInObject(result, "error", cJSON_CreateString("att"));
            cJSON_AddNumberToObject(result, "att", error - BLE_HS_ERR_ATT_BASE);
        }
    }
    s_send("ble.gatt.result", result);
    send_slots();
}
static void worker(void *unused)
{
    (void)unused;
    uint32_t idle = 0;
    request_t storage;
    request_t *request = &storage;
    for (;;) {
        if (xQueueReceive(s_requests, request, pdMS_TO_TICKS(250)) == pdTRUE) {
            run_request(request);
            memset(request->pin, 0, sizeof(request->pin));
            memset(request->value, 0, sizeof(request->value));
            idle = 0;
        }
        if (s_reset && s_connection != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_connection, BLE_ERR_REM_USER_CONN_TERM);
        if (s_reset) {
            tater_ble_scanner_stats_t stats = {0}; tater_ble_scanner_get_stats(&stats);
            tater_ble_scanner_discover(0);
            if (stats.initialized) ble_gap_conn_cancel();
        }
        if (s_connection != BLE_HS_CONN_HANDLE_NONE || tater_ble_scanner_requested() || uxQueueMessagesWaiting(s_requests)) idle = 0;
        else if (!idle) idle = now_ms();
        else if ((uint32_t)(now_ms() - idle) >= 3000) { tater_ble_stack_stop(); idle = now_ms(); }
    }
}
void tater_ble_proxy_init(tater_ble_proxy_send_t send)
{
    s_send = send;
    s_op_lock = xSemaphoreCreateMutexStatic(&s_op_lock_storage);
    s_done = xSemaphoreCreateBinaryStatic(&s_done_storage);
}
void tater_ble_proxy_reset(void)
{
    ++s_epoch;
    s_reset = true;
    tater_ble_scanner_discover(0);
}
void tater_ble_proxy_poll(void)
{
    if (!s_events || !s_send || !uxQueueMessagesWaiting(s_events)) return;
    event_t *event = heap_caps_malloc(sizeof(*event), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!event) return;
    for (int i = 0; i < EVENT_MAX && xQueueReceive(s_events, event, 0) == pdTRUE; i++) {
        cJSON *value = cJSON_CreateObject();
        cJSON_AddStringToObject(value, "t", event->notify ? "notify" : "disconnected");
        cJSON_AddStringToObject(value, "addr", event->address);
        if (event->notify) {
            cJSON_AddNumberToObject(value, "handle", event->handle);
            cJSON_AddBoolToObject(value, "ind", event->indication);
            add_encoded(value, event->value, event->length);
        } else cJSON_AddNumberToObject(value, "reason", 19);
        s_send("ble.gatt.event", value);
        if (!event->notify) send_slots();
    }
    free(event);
}
void tater_ble_proxy_request(const char *message_id, const cJSON *payload)
{
    request_t *request = heap_caps_calloc(1, sizeof(*request), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!request) return;
    request->epoch = s_epoch;
    strlcpy(request->message_id, message_id ? message_id : "", sizeof(request->message_id));
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(payload, "t");
    const cJSON *req = cJSON_GetObjectItemCaseSensitive(payload, "req");
    bool valid = cJSON_IsString(type) && strlen(type->valuestring) < sizeof(request->action)
        && cJSON_IsNumber(req) && req->valuedouble >= 0 && req->valuedouble <= UINT32_MAX
        && req->valuedouble == (double)(uint32_t)req->valuedouble;
    if (valid) request->req = (uint32_t)req->valuedouble;
    if (valid) strlcpy(request->action, type->valuestring, sizeof(request->action));
    bool scan = !strcmp(request->action, "scan");
    bool simple = scan || !strcmp(request->action, "stop_scan") || !strcmp(request->action, "slots");
    bool known = simple || !strcmp(request->action, "connect") || !strcmp(request->action, "pair") || !strcmp(request->action, "forget") || !strcmp(request->action, "disconnect") || !strcmp(request->action, "services") || !strcmp(request->action, "read") || !strcmp(request->action, "write");
    valid = valid && known;
    if (valid && scan) {
        const cJSON *duration = cJSON_GetObjectItemCaseSensitive(payload, "duration_s");
        valid = !duration || (cJSON_IsNumber(duration) && duration->valuedouble >= 5 && duration->valuedouble <= 60 && duration->valuedouble == duration->valueint);
        if (valid) request->seconds = duration ? duration->valueint : 15;
    }
    if (valid && !simple) {
        const cJSON *address = cJSON_GetObjectItemCaseSensitive(payload, "addr");
        valid = cJSON_IsString(address) && parse_address(address->valuestring, &request->peer);
        const cJSON *address_type = cJSON_GetObjectItemCaseSensitive(payload, "addr_type");
        valid = valid && (!address_type || (cJSON_IsNumber(address_type) && (address_type->valuedouble == 0 || address_type->valuedouble == 1)));
        request->peer.type = valid && address_type ? address_type->valueint : 0;
        if (valid) address_text(&request->peer, request->address);
    }
    if (valid && (!strcmp(request->action, "read") || !strcmp(request->action, "write"))) {
        const cJSON *handle = cJSON_GetObjectItemCaseSensitive(payload, "handle");
        valid = cJSON_IsNumber(handle) && handle->valuedouble >= 1 && handle->valuedouble <= 65535 && handle->valuedouble == handle->valueint;
        if (valid) request->handle = handle->valueint;
    }
    if (valid && !strcmp(request->action, "write")) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(payload, "value");
        size_t actual = 0;
        valid = cJSON_IsString(value) && strlen(value->valuestring) <= 684
            && mbedtls_base64_decode(request->value, sizeof(request->value), &actual, (unsigned char *)value->valuestring, strlen(value->valuestring)) == 0;
        request->length = actual;
        request->response = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload, "response"));
    }
    if (valid && !strcmp(request->action, "pair")) {
        const cJSON *pin = cJSON_GetObjectItemCaseSensitive(payload, "pin");
        valid = cJSON_IsString(pin) && strlen(pin->valuestring) == 6;
        if (valid) {
            for (int i = 0; i < 6; i++) if (pin->valuestring[i] < '0' || pin->valuestring[i] > '9') valid = false;
            if (valid) strlcpy(request->pin, pin->valuestring, sizeof(request->pin));
        }
    }
    if (!valid) { reply_error(request, "bad_request"); free(request); return; }
    portENTER_CRITICAL(&s_init_lock);
    bool starting = s_starting;
    if (!s_worker && !starting) s_starting = true;
    portEXIT_CRITICAL(&s_init_lock);
    if (!s_worker && !starting) {
        if (!s_requests) s_requests = xQueueCreateWithCaps(REQUEST_MAX, sizeof(request_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_events) s_events = xQueueCreateWithCaps(EVENT_MAX, sizeof(event_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_requests && s_events) xTaskCreatePinnedToCoreWithCaps(worker, "ble_proxy", 8192, NULL, 3, &s_worker, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_starting = false;
    }
    if (!s_worker || xQueueSend(s_requests, request, 0) != pdTRUE) reply_error(request, "busy");
    memset(request->pin, 0, sizeof(request->pin));
    free(request);
}
#else
void tater_ble_proxy_init(tater_ble_proxy_send_t send) { (void)send; }
void tater_ble_proxy_request(const char *id, const cJSON *payload) { (void)id; (void)payload; }
void tater_ble_proxy_poll(void) {}
void tater_ble_proxy_reset(void) {}
void tater_ble_proxy_host_config(void) {}
void tater_ble_proxy_host_reset(void) {}
#endif
