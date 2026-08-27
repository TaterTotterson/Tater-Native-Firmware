#include "boards/sat1/sat1_power_delivery.h"

#include <string.h>

#include "board.h"
#include "boards/sat1/sat1_pd_policy.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#if TATER_BOARD_SAT1_BETA_REV41

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

#define SAT1_PD_READY_BIT (1u << 0)
#define SAT1_PD_STARTUP_DELAY_MS 2000
#define SAT1_PD_RECOVERY_INTERVAL_MS 5000
#define SAT1_PD_TRANSITION_TIMEOUT_MS 2000
#define SAT1_PD_TASK_PRIORITY 18
#define SAT1_PD_TASK_CORE 1
#define SAT1_PD_STATUS_BLOCK_LEN 7
#define SAT1_PD_MAX_RX_DRAIN 16

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
static bool s_waiting_source_cap;
static bool s_transition_pending;
static uint8_t s_recovery_stage;
static TickType_t s_recovery_deadline;
static TickType_t s_transition_deadline;

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
    if (state == SAT1_PD_STATE_FALLBACK_5V || state == SAT1_PD_STATE_UNAVAILABLE
        || state == SAT1_PD_STATE_ERROR) {
        s_status.explicit_contract = false;
        s_status.contract_voltage_mv = 5000;
        s_status.contract_current_ma = 500;
    }
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
    s_status.negotiation_failed = false;
    portEXIT_CRITICAL(&s_status_lock);
}

static void status_set_request(uint8_t pdo_count, uint16_t voltage_mv)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.source_pdo_count = pdo_count;
    s_status.requested_voltage_mv = voltage_mv;
    s_status.state = SAT1_PD_STATE_NEGOTIATING;
    s_status.negotiation_failed = false;
    portEXIT_CRITICAL(&s_status_lock);
}

static void status_reset_for_negotiation(void)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.state = SAT1_PD_STATE_NEGOTIATING;
    s_status.explicit_contract = false;
    s_status.negotiation_failed = false;
    s_status.source_pdo_count = 0;
    s_status.requested_voltage_mv = 0;
    s_status.contract_voltage_mv = 5000;
    s_status.contract_current_ma = 500;
    portEXIT_CRITICAL(&s_status_lock);
    if (s_ready_event) {
        xEventGroupClearBits(s_ready_event, SAT1_PD_READY_BIT);
    }
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

static bool pd_deadline_reached(TickType_t now, TickType_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static void pd_wait_for_source_capabilities(bool reset_status)
{
    if (reset_status) {
        status_reset_for_negotiation();
    }
    s_waiting_source_cap = true;
    s_transition_pending = false;
    s_recovery_stage = 0;
    s_recovery_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SAT1_PD_RECOVERY_INTERVAL_MS);
}

static void pd_mark_request_pending(void)
{
    s_waiting_source_cap = false;
    s_transition_pending = true;
    s_transition_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SAT1_PD_TRANSITION_TIMEOUT_MS);
}

static void pd_mark_ready_or_fallback(void)
{
    s_waiting_source_cap = false;
    s_transition_pending = false;
}

static esp_err_t pd_send_request(const pd_message_t *source_capabilities)
{
    sat1_pd_fixed_selection_t selection = {0};
    if (!sat1_pd_select_beta_fixed_pdo(
            source_capabilities->objects,
            source_capabilities->object_count,
            &selection
        )) {
        ESP_LOGW(
            TAG,
            "source advertised no supported fixed PDO from %u-%u mV; legacy speaker remains on the safe 5V profile",
            SAT1_BETA_MIN_PD_MV,
            SAT1_BETA_MAX_PD_MV
        );
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint16_t current_units = selection.current_ma / 10;
    if (current_units > 0x03ff) {
        current_units = 0x03ff;
    }
    uint32_t request = ((uint32_t)selection.object_position << 28)
        | (1u << 25)
        | (1u << 24)
        | ((uint32_t)current_units << 10)
        | current_units;
    s_pending_voltage_mv = selection.voltage_mv;
    s_pending_current_ma = selection.current_ma;
    s_request_accepted = false;
    status_set_request(source_capabilities->object_count, selection.voltage_mv);
    ESP_LOGI(
        TAG,
        "requesting PDO%u: %u mV at up to %u mA (source PDOs=%u)",
        selection.object_position,
        selection.voltage_mv,
        selection.current_ma,
        source_capabilities->object_count
    );
    esp_err_t err = pd_send(PD_DATA_REQUEST, &request, 1, -1);
    if (err == ESP_OK) {
        pd_mark_request_pending();
    }
    return err;
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
            s_waiting_source_cap = false;
            if (pd_send_request(message) != ESP_OK) {
                pd_mark_ready_or_fallback();
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
        if (s_request_accepted && s_pending_voltage_mv >= SAT1_BETA_MIN_PD_MV
            && s_pending_voltage_mv <= SAT1_BETA_MAX_PD_MV) {
            pd_mark_ready_or_fallback();
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
        pd_mark_ready_or_fallback();
        status_set_state(SAT1_PD_STATE_FALLBACK_5V, true);
        break;
    case PD_CTRL_SOFT_RESET:
        s_tx_message_id = 0;
        s_last_rx_message_id = 0xff;
        s_request_accepted = false;
        ESP_ERROR_CHECK_WITHOUT_ABORT(pd_send(PD_CTRL_ACCEPT, NULL, 0, 0));
        pd_wait_for_source_capabilities(true);
        break;
    case PD_CTRL_GET_SINK_CAP: {
        // Match the official Satellite1 sink-capability response: 5V at up to 5A.
        const uint32_t sink_pdo = 500u | (100u << 10) | (1u << 26);
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

static esp_err_t fusb_read_stable_cc_level(uint8_t *level_out)
{
    if (!level_out) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t status0 = 0;
    ESP_RETURN_ON_ERROR(fusb_read_u8(FUSB_REG_STATUS0, &status0), TAG, "CC level read failed");
    uint8_t level = status0 & FUSB_STATUS0_BC_LVL;
    for (uint8_t sample = 0; sample < 5; sample++) {
        ESP_RETURN_ON_ERROR(fusb_read_u8(FUSB_REG_STATUS0, &status0), TAG, "CC stability read failed");
        if ((status0 & FUSB_STATUS0_BC_LVL) != level) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    *level_out = level;
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
    uint8_t cc1 = 0;
    ESP_RETURN_ON_ERROR(fusb_read_stable_cc_level(&cc1), TAG, "CC1 was not stable");

    ESP_RETURN_ON_ERROR(
        fusb_write_u8(
            FUSB_REG_SWITCHES0,
            FUSB_SWITCHES0_PDWN1 | FUSB_SWITCHES0_PDWN2 | FUSB_SWITCHES0_MEAS_CC2
        ),
        TAG,
        "CC2 select failed"
    );
    vTaskDelay(pdMS_TO_TICKS(5));
    uint8_t cc2 = 0;
    ESP_RETURN_ON_ERROR(fusb_read_stable_cc_level(&cc2), TAG, "CC2 was not stable");
    if (cc1_out) {
        *cc1_out = cc1;
    }
    if (cc2_out) {
        *cc2_out = cc2;
    }

    uint8_t switches0 = 0;
    uint8_t switches1 = FUSB_SWITCHES1_PD_REV2;
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

static esp_err_t fusb_initialize_controller(uint8_t *device_id_out)
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

    if (device_id_out) {
        *device_id_out = device_id;
    }
    ESP_LOGI(TAG, "FUSB302B controller configured id=0x%02x; delaying PD attach", device_id);
    return ESP_OK;
}

static esp_err_t fusb_attach(uint8_t device_id, uint8_t *cc_pin_out)
{
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

    if (cc_pin_out) {
        *cc_pin_out = selected_cc;
    }
    ESP_LOGI(TAG, "FUSB302B attached id=0x%02x CC%u (CC1=%u CC2=%u)", device_id, selected_cc, cc1, cc2);
    return ESP_OK;
}

static esp_err_t fusb_enable_auto_crc(void)
{
    uint8_t switches1 = 0;
    ESP_RETURN_ON_ERROR(fusb_read_u8(FUSB_REG_SWITCHES1, &switches1), TAG, "switches1 read failed");
    return fusb_write_u8(FUSB_REG_SWITCHES1, switches1 | FUSB_SWITCHES1_AUTO_CRC);
}

static void IRAM_ATTR fusb_irq_handler(void *arg)
{
    (void)arg;
    BaseType_t higher_priority_task_woken = pdFALSE;
    TaskHandle_t task = s_task;
    if (task) {
        vTaskNotifyGiveFromISR(task, &higher_priority_task_woken);
    }
    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t fusb_configure_irq(void)
{
    const gpio_config_t irq_config = {
        .pin_bit_mask = 1ULL << TATER_SAT1_FUSB302B_IRQ,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&irq_config), TAG, "FUSB302B IRQ pin config failed");

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "GPIO ISR service install failed");
    }
    ESP_RETURN_ON_ERROR(
        gpio_isr_handler_add(TATER_SAT1_FUSB302B_IRQ, fusb_irq_handler, NULL),
        TAG,
        "FUSB302B IRQ handler install failed"
    );
    return ESP_OK;
}

static esp_err_t pd_service_interrupt(void)
{
    uint8_t status_block[SAT1_PD_STATUS_BLOCK_LEN] = {0};
    ESP_RETURN_ON_ERROR(
        fusb_read(FUSB_REG_STATUS0A, status_block, sizeof(status_block)),
        TAG,
        "FUSB302B interrupt status read failed"
    );

    uint8_t status1 = status_block[5];
    uint8_t drained = 0;
    while (!(status1 & FUSB_STATUS1_RX_EMPTY) && drained < SAT1_PD_MAX_RX_DRAIN) {
        pd_message_t message;
        esp_err_t err = pd_read_message(&message);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "PD RX failed: %s", esp_err_to_name(err));
            ESP_ERROR_CHECK_WITHOUT_ABORT(fusb_write_u8(FUSB_REG_CONTROL1, FUSB_CONTROL1_RX_FLUSH));
            return err;
        }
        pd_handle_message(&message);
        drained++;
        ESP_RETURN_ON_ERROR(fusb_read_u8(FUSB_REG_STATUS1, &status1), TAG, "PD RX status read failed");
    }

    if (!(status1 & FUSB_STATUS1_RX_EMPTY)) {
        ESP_LOGW(TAG, "PD RX drain limit reached; flushing FIFO");
        ESP_RETURN_ON_ERROR(fusb_write_u8(FUSB_REG_CONTROL1, FUSB_CONTROL1_RX_FLUSH), TAG, "PD RX flush failed");
    }
    return ESP_OK;
}

static TickType_t pd_next_wait_ticks(void)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t deadline = 0;
    bool have_deadline = false;
    if (s_waiting_source_cap) {
        deadline = s_recovery_deadline;
        have_deadline = true;
    }
    if (s_transition_pending
        && (!have_deadline || (int32_t)(s_transition_deadline - deadline) < 0)) {
        deadline = s_transition_deadline;
        have_deadline = true;
    }
    if (!have_deadline) {
        return portMAX_DELAY;
    }
    if (pd_deadline_reached(now, deadline)) {
        return 0;
    }
    return deadline - now;
}

static void pd_run_recovery(TickType_t now)
{
    if (!s_waiting_source_cap || !pd_deadline_reached(now, s_recovery_deadline)) {
        return;
    }

    if (s_recovery_stage == 0) {
        esp_err_t err = pd_send(PD_CTRL_GET_SOURCE_CAP, NULL, 0, -1);
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "requested source capabilities after official-style 5s wait");
        } else {
            ESP_LOGW(TAG, "source capability request failed: %s", esp_err_to_name(err));
        }
        s_recovery_stage = 1;
    } else if (s_recovery_stage == 1) {
        status_reset_for_negotiation();
        esp_err_t err = fusb_flush_pd();
        if (err == ESP_OK) {
            err = pd_send(PD_CTRL_SOFT_RESET, NULL, 0, -1);
        }
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "sent one USB-PD soft reset after source-capability timeout");
        } else {
            ESP_LOGW(TAG, "USB-PD soft reset failed: %s", esp_err_to_name(err));
        }
        s_recovery_stage = 2;
    } else {
        ESP_LOGW(TAG, "USB-PD negotiation timed out after recovery; keeping the safe 5V TAS2780 profile");
        pd_mark_ready_or_fallback();
        status_set_state(SAT1_PD_STATE_FALLBACK_5V, true);
        return;
    }
    s_recovery_deadline = now + pdMS_TO_TICKS(SAT1_PD_RECOVERY_INTERVAL_MS);
}

static void pd_run_transition_timeout(TickType_t now)
{
    if (!s_transition_pending || !pd_deadline_reached(now, s_transition_deadline)) {
        return;
    }
    ESP_LOGW(TAG, "USB-PD request did not reach PS_RDY; keeping the safe 5V TAS2780 profile");
    pd_mark_ready_or_fallback();
    status_set_state(SAT1_PD_STATE_FALLBACK_5V, true);
}

static void pd_task(void *arg)
{
    (void)arg;
    uint8_t device_id = 0;
    // The official firmware configures the controller during setup, then waits
    // before it selects a CC line or starts USB-PD traffic.
    esp_err_t err = fusb_initialize_controller(&device_id);
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

    vTaskDelay(pdMS_TO_TICKS(SAT1_PD_STARTUP_DELAY_MS));

    uint8_t cc_pin = 0;
    err = fusb_attach(device_id, &cc_pin);
    if (err == ESP_OK) {
        err = fusb_configure_irq();
    }
    if (err == ESP_OK) {
        err = fusb_enable_auto_crc();
    }
    if (err == ESP_OK) {
        err = fusb_flush_pd();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PD attach failed: %s; keeping the safe 5V TAS2780 profile", esp_err_to_name(err));
        status_set_state(SAT1_PD_STATE_FALLBACK_5V, true);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    status_set_attached(cc_pin);
    pd_wait_for_source_capabilities(false);
    ESP_ERROR_CHECK_WITHOUT_ABORT(pd_service_interrupt());

    while (true) {
        TickType_t wait_ticks = pd_next_wait_ticks();
        if (ulTaskNotifyTake(pdTRUE, wait_ticks) > 0) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(pd_service_interrupt());
        }
        TickType_t now = xTaskGetTickCount();
        pd_run_recovery(now);
        pd_run_transition_timeout(now);
    }
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
    s_tx_message_id = 0;
    s_last_rx_message_id = 0xff;
    s_pending_voltage_mv = 0;
    s_pending_current_ma = 0;
    s_request_accepted = false;
    s_waiting_source_cap = false;
    s_transition_pending = false;
    s_recovery_stage = 0;
    s_initialized = true;
    portEXIT_CRITICAL(&s_status_lock);

    BaseType_t created = xTaskCreatePinnedToCore(
        pd_task,
        "sat1_pd",
        4096,
        NULL,
        SAT1_PD_TASK_PRIORITY,
        &s_task,
        SAT1_PD_TASK_CORE
    );
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
    return snapshot.explicit_contract
            && snapshot.contract_voltage_mv >= SAT1_BETA_HIGH_POWER_MIN_PD_MV
            && snapshot.contract_voltage_mv <= SAT1_BETA_MAX_PD_MV
        ? 2
        : 0;
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

#endif
