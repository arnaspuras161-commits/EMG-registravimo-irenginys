#include "ads1292.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#define ADS_MOSI   12
#define ADS_SCLK   11
#define ADS_CS     13
#define ADS_MISO   10
#define ADS_START  14
#define ADS_RESET  21
#define ADS_DRDY   9

#define CMD_RESET    0x06
#define CMD_START    0x08
#define CMD_RDATAC   0x10
#define CMD_SDATAC   0x11
#define CMD_RREG     0x20
#define CMD_WREG     0x40

#define REG_ID        0x00
#define REG_CONFIG1   0x01
#define REG_CONFIG2   0x02
#define REG_LOFF      0x03
#define REG_CH1SET    0x04
#define REG_CH2SET    0x05
#define REG_RLD_SENS  0x06
#define REG_LOFF_SENS 0x07

static const float VREF = 2.42f;
static const float GAIN = 6.0f;
static const float ADC_MAX = 8388607.0f;

static spi_device_handle_t ads_spi;

static void cs_low(void)
{
    gpio_set_level(ADS_CS, 0);
    esp_rom_delay_us(5);
}

static void cs_high(void)
{
    esp_rom_delay_us(5);
    gpio_set_level(ADS_CS, 1);
    esp_rom_delay_us(20);
}

static uint8_t spi_transfer_byte(uint8_t tx)
{
    uint8_t rx;

    spi_transaction_t t = { .length = 8, .tx_buffer = &tx, .rx_buffer = &rx };

    spi_device_transmit(ads_spi, &t);

    return rx;
}

static void ads_command(uint8_t cmd)
{
    cs_low();
    spi_transfer_byte(cmd);
    cs_high();

    esp_rom_delay_us(50);
}

static void ads_write_reg(uint8_t reg, uint8_t value)
{
    cs_low();

    spi_transfer_byte(CMD_WREG | reg);
    spi_transfer_byte(0x00);
    spi_transfer_byte(value);

    cs_high();

    esp_rom_delay_us(50);
}

static int32_t sign_extend_24(uint32_t value)
{
    if (value & 0x800000) {
        value |= 0xFF000000;
    }

    return (int32_t)value;
}

static int32_t bytes_to_raw24(uint8_t b1, uint8_t b2, uint8_t b3)
{
    uint32_t raw = (b1 << 16) | (b2 << 8) | b3;
    return sign_extend_24(raw);
}

static int16_t raw_to_uv_i16(int32_t raw)
{
    float uv = ((float)raw / ADC_MAX) * (VREF / GAIN) * 1000000.0f;

    if (uv > 32767.0f) {
        uv = 32767.0f;
    } else if (uv < -32768.0f) {
        uv = -32768.0f;
    }

    return (int16_t)uv;
}

static bool wait_drdy_fall(uint32_t timeout_us)
{
    int64_t start = esp_timer_get_time();

    while (gpio_get_level(ADS_DRDY) == 0) {
        if ((esp_timer_get_time() - start) > timeout_us) {
            return false;
        }
    }

    while (gpio_get_level(ADS_DRDY) == 1) {
        if ((esp_timer_get_time() - start) > timeout_us) {
            return false;
        }
    }

    return true;
}

static void ads_read_data_9bytes(uint8_t *data)
{
    uint8_t tx[9] = {0};

    spi_transaction_t t = { .length = 9 * 8, .tx_buffer = tx, .rx_buffer = data };

    cs_low();
    ESP_ERROR_CHECK(spi_device_transmit(ads_spi, &t));
    cs_high();
}

static void gpio_init_all(void)
{
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << ADS_CS) | (1ULL << ADS_START) | (1ULL << ADS_RESET),
        .mode = GPIO_MODE_OUTPUT
    };

    ESP_ERROR_CHECK(gpio_config(&out_conf));

    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << ADS_DRDY),
        .mode = GPIO_MODE_INPUT
    };

    ESP_ERROR_CHECK(gpio_config(&in_conf));

    gpio_set_level(ADS_CS, 1);
    gpio_set_level(ADS_START, 0);
    gpio_set_level(ADS_RESET, 1);
}

static void spi_init_ads1292(void)
{
    spi_bus_config_t buscfg = { .mosi_io_num = ADS_MOSI, .miso_io_num = ADS_MISO, .sclk_io_num = ADS_SCLK, .max_transfer_sz = 64 };

    spi_device_interface_config_t devcfg = { .clock_speed_hz = 100000, .mode = 1, .spics_io_num = -1, .queue_size = 1 };

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &ads_spi));
}

void ads1292_init(void)
{
    gpio_init_all();
    spi_init_ads1292();

    gpio_set_level(ADS_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(ADS_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(300));

    ads_command(CMD_RESET);
    vTaskDelay(pdMS_TO_TICKS(300));

    ads_command(CMD_SDATAC);
    vTaskDelay(pdMS_TO_TICKS(50));

    ads_write_reg(REG_CONFIG1, 0x03);
    ads_write_reg(REG_CONFIG2, 0xA0);
    ads_write_reg(REG_LOFF, 0x10);

    ads_write_reg(REG_CH1SET, 0x00);
    ads_write_reg(REG_CH2SET, 0x00);

    ads_write_reg(REG_RLD_SENS, 0x00);
    ads_write_reg(REG_LOFF_SENS, 0x00);

    vTaskDelay(pdMS_TO_TICKS(100));

    gpio_set_level(ADS_START, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    ads_command(CMD_START);
    ads_command(CMD_RDATAC);

    vTaskDelay(pdMS_TO_TICKS(10));
}

bool ads1292_read_sample(int16_t *ch1_uv, int16_t *ch2_uv)
{
    if (!wait_drdy_fall(100000)) {
        return false;
    }

    uint8_t data[9];

    ads_read_data_9bytes(data);

    int32_t ch1_raw = bytes_to_raw24(data[3], data[4], data[5]);
    int32_t ch2_raw = bytes_to_raw24(data[6], data[7], data[8]);

    *ch1_uv = raw_to_uv_i16(ch1_raw);
    *ch2_uv = raw_to_uv_i16(ch2_raw);

    return true;
}