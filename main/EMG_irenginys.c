#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ads1292.h"

static const char *TAG = "EMG_MAIN";

#define DEVICE_NAME "ESP32S3_ADS1292_EMG"

#define PACKET_SAMPLES 25
#define BYTES_PER_SAMPLE 4
#define BLE_PACKET_SIZE (PACKET_SAMPLES * BYTES_PER_SAMPLE)

static uint16_t conn_handle = 0;
static uint16_t emg_chr_val_handle = 0;

static bool ble_connected = false;
static bool notify_enabled = false;
static bool streaming_enabled = false;

static const ble_uuid128_t service_uuid = BLE_UUID128_INIT(
    0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
    0x93,0xF3,0xA3,0xB5,0x01,0x00,0x40,0x6E);

static const ble_uuid128_t emg_char_uuid = BLE_UUID128_INIT(
    0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
    0x93,0xF3,0xA3,0xB5,0x03,0x00,0x40,0x6E);

static const ble_uuid128_t ctrl_char_uuid = BLE_UUID128_INIT(
    0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
    0x93,0xF3,0xA3,0xB5,0x02,0x00,0x40,0x6E);

static int gatt_chr_access_cb(uint16_t conn_handle_local, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)

{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return 0;
    }

    char cmd[16] = {0};
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);

    if (len >= sizeof(cmd))
        len = sizeof(cmd) - 1;

    int rc = ble_hs_mbuf_to_flat(ctxt->om, cmd, len, NULL);

    if (rc != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    cmd[len] = '\0';
    if (strncmp(cmd, "START", 5) == 0) {
        streaming_enabled = true;
    }
    else if (strncmp(cmd, "STOP", 4) == 0) {
        streaming_enabled = false;
    }
    return 0;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service_uuid.u,

        .characteristics = (struct ble_gatt_chr_def[]) {

            { .uuid = &emg_char_uuid.u, .access_cb = gatt_chr_access_cb, .val_handle = &emg_chr_val_handle, .flags = BLE_GATT_CHR_F_NOTIFY },
            { .uuid = &ctrl_char_uuid.u, .access_cb = gatt_chr_access_cb, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { 0 }
        },
    },
    { 0 }
};

static void ble_advertise(void);

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {

        case BLE_GAP_EVENT_CONNECT:

            if (event->connect.status == 0) {
                conn_handle = event->connect.conn_handle;
                ble_connected = true;
                streaming_enabled = false;
            } else {
                ble_advertise();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:

            ble_connected = false;
            notify_enabled = false;
            streaming_enabled = false;

            ble_advertise();
            break;

        case BLE_GAP_EVENT_SUBSCRIBE:

            if (event->subscribe.attr_handle == emg_chr_val_handle) {
                notify_enabled = event->subscribe.cur_notify;
            }
            break;
    }

    return 0;
}

static void ble_advertise(void)
{
    struct ble_hs_adv_fields fields = {0};

    fields.flags =
        BLE_HS_ADV_F_DISC_GEN |
        BLE_HS_ADV_F_BREDR_UNSUP;

    fields.name = (uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;

    if (ble_gap_adv_set_fields(&fields) != 0) {
        return;
    }
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN
    };
    ble_gap_adv_start(
        BLE_OWN_ADDR_PUBLIC,
        NULL,
        BLE_HS_FOREVER,
        &adv_params,
        ble_gap_event_cb,
        NULL
    );
}


static void ble_on_sync(void)
{
    ble_svc_gap_device_name_set(DEVICE_NAME);
    ble_advertise();
}


static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_init(void)
{
    ESP_ERROR_CHECK(nimble_port_init());

    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_hs_cfg.sync_cb = ble_on_sync;

    ESP_ERROR_CHECK(ble_gatts_count_cfg(gatt_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(gatt_svcs));

    nimble_port_freertos_init(ble_host_task);
}

static void ble_notify_binary_packet(const uint8_t *packet, uint16_t len)
{
    if (!ble_connected || !notify_enabled || !streaming_enabled) {
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(packet, len);

    if (om == NULL) {
        return;
    }
    ble_gatts_notify_custom(conn_handle, emg_chr_val_handle, om);
}

static void put_i16_le(uint8_t *buf, int16_t value)
{
    buf[0] = value;
    buf[1] = value >> 8;
}

static void emg_task(void *arg)
{
    uint8_t ble_packet[BLE_PACKET_SIZE];
    int packet_index = 0;

    while (1) {

        if (!streaming_enabled) {
            packet_index = 0;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        int16_t ch1_uv, ch2_uv;
        if (!ads1292_read_sample(&ch1_uv, &ch2_uv)) {
            continue;
        }

        int byte_index = packet_index * BYTES_PER_SAMPLE;

        put_i16_le(&ble_packet[byte_index], ch1_uv);
        put_i16_le(&ble_packet[byte_index + 2], ch2_uv);
        packet_index++;

        if (packet_index >= PACKET_SAMPLES) {
            ble_notify_binary_packet(ble_packet, BLE_PACKET_SIZE);
            packet_index = 0;
        }
    }
}

static void nvs_init_safe(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
}

void app_main(void)
{
    nvs_init_safe();
    ble_init();
    vTaskDelay(pdMS_TO_TICKS(500));
    ads1292_init();
    xTaskCreate(emg_task, "emg_task", 8192, NULL, 5, NULL);
}