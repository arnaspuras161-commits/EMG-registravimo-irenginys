#include "lsm6dsox.h"

#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LSM_CS 48

#define REG_WHO_AM_I  0x0F
#define REG_CTRL1_XL  0x10
#define REG_CTRL2_G   0x11
#define REG_CTRL3_C   0x12
#define REG_OUTX_L_G  0x22

static spi_device_handle_t lsm_spi;

static void lsm_cs_low(void)
{
    gpio_set_level(LSM_CS, 0);
}

static void lsm_cs_high(void)
{
    gpio_set_level(LSM_CS, 1);
}

static uint8_t lsm_read_reg(uint8_t reg)
{
    uint8_t tx[2] = { reg | 0x80, 0x00 };
    uint8_t rx[2] = { 0 };

    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
        .rx_buffer = rx
    };

    lsm_cs_low();
    ESP_ERROR_CHECK(spi_device_transmit(lsm_spi, &t));
    lsm_cs_high();

    return rx[1];
}

static void lsm_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = { reg & 0x7F, value };

    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx
    };

    lsm_cs_low();
    ESP_ERROR_CHECK(spi_device_transmit(lsm_spi, &t));
    lsm_cs_high();
}

static void lsm_read_bytes(uint8_t reg, uint8_t *data, uint8_t len)
{
    uint8_t tx[16] = {0};
    uint8_t rx[16] = {0};

    tx[0] = reg | 0x80;

    spi_transaction_t t = {
        .length = (len + 1) * 8,
        .tx_buffer = tx,
        .rx_buffer = rx
    };

    lsm_cs_low();
    ESP_ERROR_CHECK(spi_device_transmit(lsm_spi, &t));
    lsm_cs_high();

    for (int i = 0; i < len; i++) {
        data[i] = rx[i + 1];
    }
}

static int16_t make_i16(uint8_t low, uint8_t high)
{
    return (int16_t)((high << 8) | low);
}

bool lsm6dsox_init(void)
{
    gpio_config_t cs_conf = {
        .pin_bit_mask = (1ULL << LSM_CS),
        .mode = GPIO_MODE_OUTPUT
    };

    ESP_ERROR_CHECK(gpio_config(&cs_conf));
    lsm_cs_high();

    spi_device_interface_config_t devcfg = {
    .clock_speed_hz = 1000000,
    .mode = 0,
    .spics_io_num = -1,
    .queue_size = 1
};

    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &lsm_spi));

    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t id = lsm_read_reg(REG_WHO_AM_I);

    if (id != 0x6C) {
        ESP_LOGE("IMU", "Blogas WHO_AM_I: 0x%02X", id);
        return false;
    }

    lsm_write_reg(REG_CTRL3_C, 0x44);
    lsm_write_reg(REG_CTRL1_XL, 0x40);
    lsm_write_reg(REG_CTRL2_G, 0x40);

    vTaskDelay(pdMS_TO_TICKS(50));

    return true;
}

bool lsm6dsox_read(lsm6dsox_data_t *data)
{
    uint8_t raw[12];

    lsm_read_bytes(REG_OUTX_L_G, raw, 12);

    data->gx = make_i16(raw[0], raw[1]);
    data->gy = make_i16(raw[2], raw[3]);
    data->gz = make_i16(raw[4], raw[5]);

    data->ax = make_i16(raw[6], raw[7]);
    data->ay = make_i16(raw[8], raw[9]);
    data->az = make_i16(raw[10], raw[11]);

    return true;
}