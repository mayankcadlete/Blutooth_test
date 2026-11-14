/*
  ESP-IDF (NimBLE) port of the Arduino lock controller
  Board: ESP32 (NodeMCU-32S)

  - Advertises as "ESP32_<DEVICE_ID>"
  - Nordic UART Service (same UUIDs as before, so index.html works unchanged)
  - Phone must send  AUTH:<SECRET_KEY>  first, otherwise commands are refused
  - GPIO2  = onboard LED   (command 2:1 / 2:0)
  - GPIO27 = relay -> lock (command 27:1 = one short unlock pulse)

  Use the SAME DEVICE_ID and SECRET_KEY as the ones in your QR code.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

// ---------- CHANGE THESE TWO ----------
#define DEVICE_ID   "A7K3"        // 3-8 letters/numbers
#define SECRET_KEY  "k7Q2xM9p"    // 6-12 letters/numbers
// --------------------------------------

#define DEVICE_NAME "ESP32_" DEVICE_ID

#define LED_PIN              2
#define LOCK_PIN             27
#define LOCK_PULSE_MS        400      // relay ON time per unlock (keep under 500 ms)
#define LOCK_COOLDOWN_MS     10000    // minimum gap between two unlocks

static const char *TAG = "LOCK";

// ------------------------- GPIO helpers -------------------------
static bool is_allowed_pin(int pin) { return pin == LED_PIN || pin == LOCK_PIN; }
static bool is_active_low(int pin)  { return pin == LOCK_PIN; }   // relay: LOW = ON

// on = 1 means "switched ON", whatever the real pin level is
static void set_output(int pin, int on)
{
    gpio_set_level((gpio_num_t)pin, is_active_low(pin) ? !on : on);
}

static void init_outputs(void)
{
    const int pins[] = {LED_PIN, LOCK_PIN};
    for (int i = 0; i < 2; i++) {
        gpio_reset_pin((gpio_num_t)pins[i]);
        set_output(pins[i], 0);                                   // OFF level first (relay stays off)
        gpio_set_direction((gpio_num_t)pins[i], GPIO_MODE_OUTPUT);
        set_output(pins[i], 0);
    }
}

// ------------------------- BLE state -------------------------
static uint8_t  own_addr_type;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t tx_handle;
static bool     authorized = false;

static void start_advertising(void);

static void send_text(const char *text)
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(text, strlen(text));
    if (!om) return;
    int rc = ble_gatts_notify_custom(conn_handle, tx_handle, om);   // om is consumed
    if (rc != 0) ESP_LOGW(TAG, "notify failed: %d", rc);
}

// ------------------------- Lock pulse -------------------------
static esp_timer_handle_t lock_timer;
static volatile bool lock_active = false;
static int64_t last_unlock_us = 0;

static void lock_timer_cb(void *arg)
{
    set_output(LOCK_PIN, 0);
    lock_active = false;
    ESP_LOGI(TAG, "Unlock pulse finished");
    send_text("OK 27:0");
}

// ------------------------- Command handling -------------------------
static void handle_message(char *msg)
{
    // trim spaces / newlines
    while (isspace((unsigned char)*msg)) msg++;
    size_t n = strlen(msg);
    while (n > 0 && isspace((unsigned char)msg[n - 1])) msg[--n] = '\0';
    if (n == 0) return;

    // 1) Authentication:  AUTH:<key>
    if (strncmp(msg, "AUTH:", 5) == 0) {
        if (strcmp(msg + 5, SECRET_KEY) == 0) {
            authorized = true;
            ESP_LOGI(TAG, "Phone authorized");
            send_text("OK AUTH");
        } else {
            authorized = false;
            ESP_LOGW(TAG, "Wrong key!");
            send_text("ERR auth");
        }
        return;
    }

    // 2) Everything else needs authorization
    if (!authorized) {
        send_text("ERR not authorized");
        return;
    }
    ESP_LOGI(TAG, "Phone says: %s", msg);

    // Command format:  <pin>:<0|1>
    char *sep = strchr(msg, ':');
    if (!sep) {
        send_text("ERR bad command");
        return;
    }
    int pin = atoi(msg);
    int val = atoi(sep + 1);
    if (!is_allowed_pin(pin) || (val != 0 && val != 1)) {
        send_text("ERR pin not allowed or bad value");
        return;
    }

    if (pin == LOCK_PIN && val == 1) {
        if (lock_active) { send_text("ERR lock busy"); return; }
        int64_t now = esp_timer_get_time();
        if (last_unlock_us != 0 && (now - last_unlock_us) < (int64_t)LOCK_COOLDOWN_MS * 1000) {
            send_text("ERR wait 10 s between unlocks");
            return;
        }
        set_output(LOCK_PIN, 1);
        lock_active = true;
        last_unlock_us = now;
        esp_timer_start_once(lock_timer, (uint64_t)LOCK_PULSE_MS * 1000);
        ESP_LOGI(TAG, "Unlock pulse started");
        send_text("OK 27:1");
    } else {
        set_output(pin, val);
        if (pin == LOCK_PIN) lock_active = false;
        char reply[24];
        snprintf(reply, sizeof(reply), "OK %d:%d", pin, val);
        ESP_LOGI(TAG, "GPIO%d -> %d", pin, val);
        send_text(reply);
    }
}

// ------------------------- GATT (Nordic UART) -------------------------
// 6E400001-B5A3-F393-E0A9-E50E24DCCA9E (bytes written in reverse order)
static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
// 6E400002-... phone -> ESP32
static const ble_uuid128_t rx_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
// 6E400003-... ESP32 -> phone
static const ble_uuid128_t tx_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

static int rx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    char buf[64];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &len) != 0) return BLE_ATT_ERR_UNLIKELY;
    buf[len] = '\0';
    handle_message(buf);
    return 0;
}

static int tx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return 0;   // TX is notify-only
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &rx_uuid.u,
                .access_cb = rx_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &tx_uuid.u,
                .access_cb = tx_access,
                .val_handle = &tx_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

// ------------------------- GAP (advertising + connection events) -------------------------
static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            conn_handle = event->connect.conn_handle;
            authorized = false;                    // every new connection must authenticate
            ESP_LOGI(TAG, "Phone connected (not authorized yet)");
        } else {
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        authorized = false;
        ESP_LOGI(TAG, "Phone disconnected - advertising again");
        start_advertising();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        break;

    default:
        break;
    }
    return 0;
}

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    const char *name = ble_svc_gap_device_name();
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_set_fields failed: %d", rc); return; }

    struct ble_hs_adv_fields rsp;                  // scan response carries the service UUID
    memset(&rsp, 0, sizeof(rsp));
    rsp.uuids128 = (ble_uuid128_t *)&svc_uuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) { ESP_LOGE(TAG, "adv_rsp_set_fields failed: %d", rc); return; }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event, NULL);
    if (rc != 0) { ESP_LOGE(TAG, "adv_start failed: %d", rc); return; }

    ESP_LOGI(TAG, "Advertising as \"%s\"", name);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    assert(rc == 0);
    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE reset, reason=%d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();                 // returns only when nimble_port_stop() is called
    nimble_port_freertos_deinit();
}

// ------------------------- main -------------------------
void app_main(void)
{
    // NVS is required by the BLE stack
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    init_outputs();

    const esp_timer_create_args_t timer_args = {
        .callback = lock_timer_cb,
        .name = "lock_pulse",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &lock_timer));

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(gatt_svcs) == 0 ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(ble_gatts_add_svcs(gatt_svcs) == 0 ? ESP_OK : ESP_FAIL);
    ble_svc_gap_device_name_set(DEVICE_NAME);

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "Started. Device ID: %s", DEVICE_ID);
}
