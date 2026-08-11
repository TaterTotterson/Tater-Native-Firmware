#include "boards/sat1/sat1_power_delivery.h"

#include <string.h>

#include "boards/sat1/board_sat1.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#define FUSB_REG_DEVICE_ID 0x01
#define FUSB_REG_SWITCHES0 0x02
#define FUSB_REG_SWITCHES1 0x03
#define FUSB_REG_MEASURE 0x04
#define FUSB_REG_CONTROL0 0x06
#define FUSB_REG_CONTROL1 0x07
#define FUSB_REG_CONTROL3 0x09
#define FUSB_REG_MASK 0x0A
#define FUSB_REG_POWER 0x0B
#define FUSB_REG_RESET 0x0C
#define FUSB_REG_MASKA 0x0E
#define FUSB_REG_MASKB 0x0F
#define FUSB_REG_STATUS0A 0x3C
#define FUSB_REG_STATUS0 0x40
#define FUSB_REG_STATUS1 0x41
#define FUSB_REG_FIFO 0x43

#define FUSB_SWITCHES0_PDWN1 (1u << 0)
#define FUSB_SWITCHES0_PDWN2 (1u << 1)
#define FUSB_SWITCHES0_MEAS_CC1 (1u << 2)
#define FUSB_SWITCHES0_MEAS_CC2 (1u << 3)
#define FUSB_SWITCHES1_TXCC1 (1u << 0)
#define FUSB_SWITCHES1_TXCC2 (1u << 1)
#define FUSB_SWITCHES1_AUTO_CRC (1u << 2)
#define FUSB_SWITCHES1_PD_REV2 (1u << 5)
#define FUSB_CONTROL0_INT_MASK (1u << 5)
#define FUSB_CONTROL0_TX_FLUSH (1u << 6)
#define FUSB_CONTROL1_RX_FLUSH (1u << 2)
#define FUSB_CONTROL3_AUTO_RETRY (1u << 0)
#define FUSB_CONTROL3_RETRIES_3 (3u << 1)
#define FUSB_RESET_PD (1u << 1)
#define FUSB_RESET_SOFTWARE (1u << 0)
#define FUSB_STATUS0_BC_LVL 0x03
#define FUSB_STATUS1_RX_EMPTY (1u << 5)
#define FUSB_FIFO_RX_TOKEN_MASK 0xE0
#define FUSB_FIFO_RX_SOP 0xE0

#define FUSB_TX_TOKEN_SOP1 0x12
#define FUSB_TX_TOKEN_SOP2 0x13
#define FUSB_TX_TOKEN_PACKSYM 0x80
#define FUSB_TX_TOKEN_JAM_CRC 0xFF
#define FUSB_TX_TOKEN_EOP 0x14
#define FUSB_TX_TOKEN_TXOFF 0xFE
#define FUSB_TX_TOKEN_TXON 0xA1

#define PD_CTRL_GOODCRC 1
#define PD_CTRL_ACCEPT 3
#define PD_CTRL_REJECT 4
#define PD_CTRL_PS_RDY 6
#define PD_CTRL_GET_SOURCE_CAP 7
#define PD_CTRL_GET_SINK_CAP 8
#define PD_CTRL_WAIT 12
#define PD_CTRL_SOFT_RESET 13
#define PD_CTRL_NOT_SUPPORTED 16
#define PD_DATA_SOURCE_CAP 1
#define PD_DATA_REQUEST 2
#define PD_DATA_SINK_CAP 4
#define PD_SPEC_REV2 1
#define PD_MAX_OBJECTS 7
#define PD_MAX_REQUEST_MV 20000

#define SAT1_PD_READY_BIT (1u << 0)
#define SAT1_PD_NEGOTIATION_TIMEOUT_MS 6500
#define SAT1_PD_GET_SOURCE_CAP_INTERVAL_MS 1800

typedef struct {
    uint8_t type;
    uint8_t spec_revision;
    uint8_t message_id;
    uint8_t object_count;
    bool extended;
    uint32_t objects[PD_MAX_OBJECTS];
} pd_message_t;

static const char *TAG = "sat1_pd";
static sat1_pd_i2c_read_fn s_read;
static sat1_pd_i2c_write_fn s_write;
static EventGroupHandle_t s_ready_event;
static TaskHandle_t s_task;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static sat1_pd_status_t s_status = {
    .state = SAT1_PD_STATE_UNINITIALIZED,
    .contract_voltage_mv = 5000,
    .contract_current_ma = 500,
};
static bool s_initialized;
static uint8_t s_tx_message_id;
static uint8_t s_last_rx_message_id = 0xff;
static uint16_t s_pending_voltage_mv;
static uint16_t s_pending_current_ma;
static bool s_request_accepted;

static esp_err_t fusb_read(uint8_t reg, uint8_t *data, size_t len)
{
    if (!s_read || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return s_read(TATER_SAT1_FUSB302B_I2C_ADDR, reg, data, len);
}

static esp_err_t fusb_write(uint8_t reg, const uint8_t *data, size_t len)
{
    if (!s_write || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return s_write(TATER_SAT1_FUSB302B_I2C_ADDR, reg, data, len);
}

static esp_err_t fusb_read_u8(uint8_t reg, uint8_t *value)
{
    return fusb_read(reg, value, 1);
}

static esp_err_t fusb_write_u8(uint8_t reg, uint8_t value)
{
    return fusb_write(reg, &value, 1);
}

static void status_set_state(sat1_pd_state_t state, bool failed)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.state = state;
    s_status.negotiation_failed = failed;
    portEXIT_CRITICAL(&s_status_lock);
    if (state == SAT1_PD_STATE_EXPLICIT_CONTRACT || state == SAT1_PD_STATE_FALLBACK_5V
        || state == SAT1_PD_STATE_UNAVAILABLE || state == SAT1_PD_STATE_ERROR) {
        if (s_ready_event) {
            xEventGroupSetBits(s_ready_event, SAT1_PD_READY_BIT);
        }
    }
}

static void status_set_controller(uint8_t device_id)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.controller_present = true;
    s_status.device_id = device_id;
    portEXIT_CRITICAL(&s_status_lock);
}

static void status_set_attached(uint8_t cc_pin)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.attached = true;
    s_status.cc_pin = cc_pin;
    s_status.state = SAT1_PD_STATE_NEGOTIATING;
    portEXIT_CRITICAL(&s_status_lock);
}

static void status_set_request(uint8_t pdo_count, uint16_t voltage_mv)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.source_pdo_count = pdo_count;
    s_status.requested_voltage_mv = voltage_mv;
    portEXIT_CRITICAL(&s_status_lock);
}

static void status_set_contract(void)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.explicit_contract = true;
    s_status.negotiation_failed = false;
    s_status.contract_voltage_mv = s_pending_voltage_mv;
    s_status.contract_current_ma = s_pending_current_ma;
    s_status.state = SAT1_PD_STATE_EXPLICIT_CONTRACT;
    portEXIT_CRITICAL(&s_status_lock);
    if (s_ready_event) {
        xEventGroupSetBits(s_ready_event, SAT1_PD_READY_BIT);
    }
}

static uint16_t pd_header(uint8_t type, uint8_t object_count, uint8_t message_id)
{
    return ((uint16_t)type & 0x1f)
        | ((uint16_t)PD_SPEC_REV2 << 6)
        | (((uint16_t)message_id & 0x07) << 9)
        | (((uint16_t)object_count & 0x07) << 12);
}

static esp_err_t pd_send(uint8_t type, const uint32_t *objects, uint8_t object_count, int message_id_override)
{
    if (object_count > PD_MAX_OBJECTS || (object_count > 0 && !objects)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t message_id = message_id_override >= 0 ? (uint8_t)message_id_override : s_tx_message_id;
    uint16_t header = pd_header(type, object_count, message_id);
    uint8_t packet[40];
    size_t offset = 0;
    packet[offset++] = FUSB_TX_TOKEN_SOP1;
    packet[offset++] = FUSB_TX_TOKEN_SOP1;
    packet[offset++] = FUSB_TX_TOKEN_SOP1;
    packet[offset++] = FUSB_TX_TOKEN_SOP2;
    packet[offset++] = FUSB_TX_TOKEN_PACKSYM | (uint8_t)(2 + (object_count * 4));
    packet[offset++] = (uint8_t)(header & 0xff);
    packet[offset++] = (uint8_t)(header >> 8);
    for (uint8_t i = 0; i < object_count; i++) {
        uint32_t value = objects[i];
        packet[offset++] = (uint8_t)(value & 0xff);
        packet[offset++] = (uint8_t)((value >> 8) & 0xff);
        packet[offset++] = (uint8_t)((value >> 16) & 0xff);
        packet[offset++] = (uint8_t)((value >> 24) & 0xff);
    }
    packet[offset++] = FUSB_TX_TOKEN_JAM_CRC;
    packet[offset++] = FUSB_TX_TOKEN_EOP;
    packet[offset++] = FUSB_TX_TOKEN_TXOFF;
    packet[offset++] = FUSB_TX_TOKEN_TXON;
    return fusb_write(FUSB_REG_FIFO, packet, offset);
}

static esp_err_t pd_read_message(pd_message_t *message)
{
    if (!message) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(message, 0, sizeof(*message));

    uint8_t token = 0;
    ESP_RETURN_ON_ERROR(fusb_read(FUSB_REG_FIFO, &token, 1), TAG, "fifo token read failed");
    if ((token & FUSB_FIFO_RX_TOKEN_MASK) != FUSB_FIFO_RX_SOP) {
        ESP_LOGW(TAG, "discarding unexpected RX token 0x%02x", token);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t raw_header[2] = {0};
    ESP_RETURN_ON_ERROR(fusb_read(FUSB_REG_FIFO, raw_header, sizeof(raw_header)), TAG, "fifo header read failed");
    uint16_t header = (uint16_t)raw_header[0] | ((uint16_t)raw_header[1] << 8);
    message->type = header & 0x1f;
    message->spec_revision = (header >> 6) & 0x03;
    message->message_id = (header >> 9) & 0x07;
    message->object_count = (header >> 12) & 0x07;
    message->extended = (header & 0x8000) != 0;
    if (message->object_count > PD_MAX_OBJECTS) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (message->object_count > 0) {
        uint8_t raw_objects[PD_MAX_OBJECTS * 4] = {0};
        size_t object_bytes = message->object_count * 4;
        ESP_RETURN_ON_ERROR(fusb_read(FUSB_REG_FIFO, raw_objects, object_bytes), TAG, "fifo objects read failed");
        for (uint8_t i = 0; i < message->object_count; i++) {
            const uint8_t *value = &raw_objects[i * 4];
            message->objects[i] = (uint32_t)value[0]
                | ((uint32_t)value[1] << 8)
                | ((uint32_t)value[2] << 16)
                | ((uint32_t)value[3] << 24);
        }
    }

    uint8_t crc[4];
    return fusb_read(FUSB_REG_FIFO, crc, sizeof(crc));
}

static esp_err_t pd_send_request(const pd_message_t *source_capabilities)
{
    uint8_t selected_position = 0;
    uint16_t selected_voltage_mv = 0;
    uint16_t selected_current_ma = 0;
    for (uint8_t i = 0; i < source_capabilities->object_count; i++) {
        uint32_t pdo = source_capabilities->objects[i];
        if ((pdo >> 30) != 0) {
            continue;
        }
        uint16_t voltage_mv = (uint16_t)(((pdo >> 10) & 0x03ff) * 50);
        uint16_t current_ma = (uint16_t)((pdo & 0x03ff) * 10);
        if (voltage_mv <= PD_MAX_REQUEST_MV && voltage_mv > selected_voltage_mv) {
            selected_position = i + 1;
            selected_voltage_mv = voltage_mv;
            selected_current_ma = current_ma;
        }
    }
    if (selected_position == 0) {
        ESP_LOGW(TAG, "source advertised no supported fixed PDO");
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint16_t current_units = selected_current_ma / 10;
    if (current_units > 0x03ff) {
        current_units = 0x03ff;
    }
    uint32_t request = ((uint32_t)selected_position << 28)
        | (1u << 25)
        | (1u << 24)
        | ((uint32_t)current_units << 10)
        | current_units;
    s_pending_voltage_mv = selected_voltage_mv;
    s_pending_current_ma = selected_current_ma;
    s_request_accepted = false;
    status_set_request(source_capabilities->object_count, selected_voltage_mv);
    ESP_LOGI(
        TAG,
        "requesting PDO%u: %u mV at up to %u mA (source PDOs=%u)",
        selected_position,
        selected_voltage_mv,
        selected_current_ma,
        source_capabilities->object_count
    );
    return pd_send(PD_DATA_REQUEST, &request, 1, -1);
}

static void pd_handle_message(const pd_message_t *message)
{
    if (!message || message->extended) {
        return;
    }
    if (message->object_count == 0 && message->type == PD_CTRL_GOODCRC) {
        s_tx_message_id = (s_tx_message_id + 1) & 0x07;
        return;
    }
    if (message->message_id == s_last_rx_message_id) {
        return;
    }
    s_last_rx_message_id = message->message_id;

    if (message->object_count > 0) {
        if (message->type == PD_DATA_SOURCE_CAP) {
            if (pd_send_request(message) != ESP_OK) {
                status_set_state(SAT1_PD_STATE_FALLBACK_5V, true);
            }
        }
        return;
    }

    switch (message->type) {
    case PD_CTRL_ACCEPT:
        s_request_accepted = true;
        ESP_LOGI(TAG, "power request accepted; waiting for PS_RDY");
        break;
    case PD_CTRL_PS_RDY:
        if (s_request_accepted && s_pending_voltage_mv >= 5000) {
            status_set_contract();
            ESP_LOGI(
                TAG,
                "explicit USB-PD contract ready: %u mV at up to %u mA; TAS2780 mode=%u",
                s_pending_voltage_mv,
                s_pending_current_ma,
                sat1_pd_recommended_tas_mode()
            );
        }
        break;
    case PD_CTRL_REJECT:
    case PD_CTRL_WAIT:
        ESP_LOGW(TAG, "source declined power request (response=%u); using safe 5V profile", message->type);
        status_set_state(SAT1_PD_STATE_FALLBACK_5V, true);
        break;
    case PD_CTRL_SOFT_RESET:
        s_tx_message_id = 0;
        s_last_rx_message_id = 0xff;
        s_request_accepted = false;
        ESP_ERROR_CHECK_WITHOUT_ABORT(pd_send(PD_CTRL_ACCEPT, NULL, 0, 0));
        break;
    case PD_CTRL_GET_SINK_CAP: {
        const uint32_t sink_pdo = 300u | (100u << 10) | (1u << 26);
        ESP_ERROR_CHECK_WITHOUT_ABORT(pd_send(PD_DATA_SINK_CAP, &sink_pdo, 1, -1));
        break;
    }
    default:
        if (message->spec_revision >= PD_SPEC_REV2) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(pd_send(PD_CTRL_NOT_SUPPORTED, NULL, 0, -1));
        }
        break;
    }
}

static esp_err_t fusb_flush_pd(void)
{
    ESP_RETURN_ON_ERROR(fusb_write_u8(FUSB_REG_CONTROL0, FUSB_CONTROL0_TX_FLUSH), TAG, "TX flush failed");
    ESP_RETURN_ON_ERROR(fusb_write_u8(FUSB_REG_CONTROL1, FUSB_CONTROL1_RX_FLUSH), TAG, "RX flush failed");
    ESP_RETURN_ON_ERROR(fusb_write_u8(FUSB_REG_RESET, FUSB_RESET_PD), TAG, "PD reset failed");
    s_tx_message_id = 0;
    s_last_rx_message_id = 0xff;
    s_request_accepted = false;
    return ESP_OK;
}

static esp_err_t fusb_select_cc(uint8_t *selected_cc, uint8_t *cc1_out, uint8_t *cc2_out)
{
    ESP_RETURN_ON_ERROR(
        fusb_write_u8(
            FUSB_REG_SWITCHES0,
            FUSB_SWITCHES0_PDWN1 | FUSB_SWITCHES0_PDWN2 | FUSB_SWITCHES0_MEAS_CC1
        ),
        TAG,
        "CC1 select failed"
    );
    ESP_RETURN_ON_ERROR(fusb_write_u8(FUSB_REG_SWITCHES1, FUSB_SWITCHES1_PD_REV2), TAG, "PD revision failed");
    ESP_RETURN_ON_ERROR(fusb_write_u8(FUSB_REG_MEASURE, 49), TAG, "CC measure threshold failed");
    vTaskDelay(pdMS_TO_TICKS(5));
    uint8_t status0 = 0;
    ESP_RETURN_ON_ERROR(fusb_read_u8(FUSB_REG_STATUS0, &status0), TAG, "CC1 read failed");
    uint8_t cc1 = status0 & FUSB_STATUS0_BC_LVL;

    ESP_RETURN_ON_ERROR(
        fusb_write_u8(
            FUSB_REG_SWITCHES0,
            FUSB_SWITCHES0_PDWN1 | FUSB_SWITCHES0_PDWN2 | FUSB_SWITCHES0_MEAS_CC2
        ),
        TAG,
        "CC2 select failed"
    );
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(fusb_read_u8(FUSB_REG_STATUS0, &status0), TAG, "CC2 read failed");
    uint8_t cc2 = status0 & FUSB_STATUS0_BC_LVL;
    if (cc1_out) {
        *cc1_out = cc1;
    }
    if (cc2_out) {
        *cc2_out = cc2;
    }

    uint8_t switches0 = 0;
    uint8_t switches1 = FUSB_SWITCHES1_PD_REV2 | FUSB_SWITCHES1_AUTO_CRC;
    if (cc1 > 0 && cc2 == 0) {
        *selected_cc = 1;
        switches0 = FUSB_SWITCHES0_PDWN1 | FUSB_SWITCHES0_PDWN2 | FUSB_SWITCHES0_MEAS_CC1;
        switches1 |= FUSB_SWITCHES1_TXCC1;
    } else if (cc2 > 0 && cc1 == 0) {
        *selected_cc = 2;
        switches0 = FUSB_SWITCHES0_PDWN1 | FUSB_SWITCHES0_PDWN2 | FUSB_SWITCHES0_MEAS_CC2;
        switches1 |= FUSB_SWITCHES1_TXCC2;
    } else {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(fusb_write_u8(FUSB_REG_SWITCHES0, switches0), TAG, "active CC select failed");
    return fusb_write_u8(FUSB_REG_SWITCHES1, switches1);
}

static esp_err_t fusb_initialize(uint8_t *device_id_out, uint8_t *cc_pin_out)
{
    ESP_RETURN_ON_ERROR(fusb_write_u8(FUSB_REG_RESET, FUSB_RESET_SOFTWARE), TAG, "software reset failed");
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t device_id = 0;
    ESP_RETURN_ON_ERROR(fusb_read_u8(FUSB_REG_DEVICE_ID, &device_id), TAG, "device id read failed");
    if (device_id != 0x81 && device_id != 0x91) {
        ESP_LOGW(TAG, "unexpected FUSB302B device id 0x%02x", device_id);
        return ESP_ERR_NOT_FOUND;
    }
    status_set_controller(device_id);

    ESP_RETURN_ON_ERROR(fusb_write_u8(FUSB_REG_MASK, 0x51), TAG, "interrupt mask failed");
    ESP_RETURN_ON_ERROR(fusb_write_u8(FUSB_REG_MASKA, 0x00), TAG, "interrupt mask A failed");
    ESP_RETURN_ON_ERROR(fusb_write_u8(FUSB_REG_MASKB, 0x00), TAG, "interrupt mask B failed");
    uint8_t control0 = 0;
    ESP_RETURN_ON_ERROR(fusb_read_u8(FUSB_REG_CONTROL0, &control0), TAG, "control0 read failed");
    ESP_RETURN_ON_ERROR(
        fusb_write_u8(FUSB_REG_CONTROL0, control0 & ~FUSB_CONTROL0_INT_MASK),
        TAG,
        "global interrupt enable failed"
    );
    ESP_RETURN_ON_ERROR(
        fusb_write_u8(FUSB_REG_CONTROL3, FUSB_CONTROL3_RETRIES_3 | FUSB_CONTROL3_AUTO_RETRY),
        TAG,
        "auto retry setup failed"
    );
    ESP_RETURN_ON_ERROR(fusb_write_u8(FUSB_REG_POWER, 0x0f), TAG, "power blocks enable failed");

    uint8_t selected_cc = 0;
    uint8_t cc1 = 0;
    uint8_t cc2 = 0;
    esp_err_t cc_err = ESP_ERR_NOT_FOUND;
    for (uint8_t attempt = 0; attempt < 10 && cc_err != ESP_OK; attempt++) {
        cc_err = fusb_select_cc(&selected_cc, &cc1, &cc2);
        if (cc_err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    if (cc_err != ESP_OK) {
        ESP_LOGW(TAG, "USB-C sink attachment not detected (CC1=%u CC2=%u)", cc1, cc2);
        return cc_err;
    }
    ESP_RETURN_ON_ERROR(fusb_flush_pd(), TAG, "PD block reset failed");
    if (device_id_out) {
        *device_id_out = device_id;
    }
    if (cc_pin_out) {
        *cc_pin_out = selected_cc;
    }
    ESP_LOGI(TAG, "FUSB302B ready id=0x%02x CC%u (CC1=%u CC2=%u)", device_id, selected_cc, cc1, cc2);
    return ESP_OK;
}

static void pd_task(void *arg)
{
    (void)arg;
    uint8_t device_id = 0;
    uint8_t cc_pin = 0;
    esp_err_t err = fusb_initialize(&device_id, &cc_pin);
    if (err != ESP_OK) {
        sat1_pd_status_t snapshot = {0};
        sat1_pd_status_snapshot(&snapshot);
        if (snapshot.controller_present) {
            ESP_LOGW(TAG, "PD setup failed: %s; keeping the safe 5V TAS2780 profile", esp_err_to_name(err));
            status_set_state(SAT1_PD_STATE_FALLBACK_5V, true);
        } else {
            ESP_LOGW(TAG, "FUSB302B unavailable: %s; keeping the safe 5V TAS2780 profile", esp_err_to_name(err));
            status_set_state(SAT1_PD_STATE_UNAVAILABLE, true);
        }
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    status_set_attached(cc_pin);
    TickType_t started = xTaskGetTickCount();
    TickType_t last_request = started - pdMS_TO_TICKS(SAT1_PD_GET_SOURCE_CAP_INTERVAL_MS);
    uint8_t source_cap_requests = 0;
    while (true) {
        uint8_t status_block[7] = {0};
        ESP_ERROR_CHECK_WITHOUT_ABORT(fusb_read(FUSB_REG_STATUS0A, status_block, sizeof(status_block)));

        uint8_t status1 = 0;
        if (fusb_read_u8(FUSB_REG_STATUS1, &status1) == ESP_OK) {
            for (uint8_t drained = 0; !(status1 & FUSB_STATUS1_RX_EMPTY) && drained < 8; drained++) {
                pd_message_t message;
                err = pd_read_message(&message);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "PD RX failed: %s", esp_err_to_name(err));
                    ESP_ERROR_CHECK_WITHOUT_ABORT(fusb_write_u8(FUSB_REG_CONTROL1, FUSB_CONTROL1_RX_FLUSH));
                    break;
                }
                pd_handle_message(&message);
                if (fusb_read_u8(FUSB_REG_STATUS1, &status1) != ESP_OK) {
                    break;
                }
            }
        }

        sat1_pd_status_t snapshot = {0};
        sat1_pd_status_snapshot(&snapshot);
        if (snapshot.state == SAT1_PD_STATE_EXPLICIT_CONTRACT
            || snapshot.state == SAT1_PD_STATE_FALLBACK_5V) {
            break;
        }

        TickType_t now = xTaskGetTickCount();
        if (snapshot.requested_voltage_mv == 0 && source_cap_requests < 3
            && (now - last_request) >= pdMS_TO_TICKS(SAT1_PD_GET_SOURCE_CAP_INTERVAL_MS)) {
            err = pd_send(PD_CTRL_GET_SOURCE_CAP, NULL, 0, -1);
            if (err == ESP_OK) {
                source_cap_requests++;
                last_request = now;
                ESP_LOGD(TAG, "requested source capabilities attempt=%u", source_cap_requests);
            }
        }
        if ((now - started) >= pdMS_TO_TICKS(SAT1_PD_NEGOTIATION_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "USB-PD negotiation timed out; keeping the safe 5V TAS2780 profile");
            status_set_state(SAT1_PD_STATE_FALLBACK_5V, true);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t sat1_pd_init(sat1_pd_i2c_read_fn read_fn, sat1_pd_i2c_write_fn write_fn)
{
    if (!read_fn || !write_fn) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized) {
        return ESP_OK;
    }
    s_read = read_fn;
    s_write = write_fn;
    s_ready_event = xEventGroupCreate();
    if (!s_ready_event) {
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_status_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = SAT1_PD_STATE_NEGOTIATING;
    s_status.contract_voltage_mv = 5000;
    s_status.contract_current_ma = 500;
    s_initialized = true;
    portEXIT_CRITICAL(&s_status_lock);

    BaseType_t created = xTaskCreatePinnedToCore(pd_task, "sat1_pd", 4096, NULL, 5, &s_task, 0);
    if (created != pdPASS) {
        portENTER_CRITICAL(&s_status_lock);
        s_status.state = SAT1_PD_STATE_ERROR;
        s_status.negotiation_failed = true;
        portEXIT_CRITICAL(&s_status_lock);
        xEventGroupSetBits(s_ready_event, SAT1_PD_READY_BIT);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool sat1_pd_wait_ready(uint32_t timeout_ms)
{
    if (!s_ready_event) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(
        s_ready_event,
        SAT1_PD_READY_BIT,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(timeout_ms)
    );
    return (bits & SAT1_PD_READY_BIT) != 0;
}

bool sat1_pd_status_snapshot(sat1_pd_status_t *out)
{
    if (!out || !s_initialized) {
        return false;
    }
    portENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    return true;
}

uint8_t sat1_pd_recommended_tas_mode(void)
{
    sat1_pd_status_t snapshot = {0};
    if (!sat1_pd_status_snapshot(&snapshot)) {
        return 0;
    }
    return snapshot.explicit_contract && snapshot.contract_voltage_mv >= 9000 ? 2 : 0;
}

const char *sat1_pd_state_name(sat1_pd_state_t state)
{
    switch (state) {
    case SAT1_PD_STATE_NEGOTIATING:
        return "negotiating";
    case SAT1_PD_STATE_EXPLICIT_CONTRACT:
        return "explicit_contract";
    case SAT1_PD_STATE_FALLBACK_5V:
        return "fallback_5v";
    case SAT1_PD_STATE_UNAVAILABLE:
        return "unavailable";
    case SAT1_PD_STATE_ERROR:
        return "error";
    default:
        return "uninitialized";
    }
}
