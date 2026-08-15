#include "sdcard.h"

#include <string.h>
#include <sys/stat.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#define PIN_SD_CLK 2
#define PIN_SD_CMD 1
#define PIN_SD_D0 3

#define EXPANDER_ADDR 0x20
#define EXPANDER_REG_OUTPUT 0x01
#define EXPANDER_REG_CONFIG 0x03
#define EXPANDER_SD_CS_BIT 0x80  // EXIO7 = the card's DAT3 / CS

static const char *TAG = "sdcard";

static sdmmc_card_t *s_card;
static bool s_ready;

// DAT3 must sit high through SDMMC init. The expander powers up high-Z into
// a pull-up, which usually suffices — but the panel reset drives other pins
// on the same chip, so make it explicit rather than lucky.
static void release_card_cs(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = EXPANDER_ADDR,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) {
        return;
    }
    uint8_t reg = EXPANDER_REG_OUTPUT, out = 0xFF, dir = 0xFF;
    if (i2c_master_transmit_receive(dev, &reg, 1, &out, 1, 50) == ESP_OK) {
        reg = EXPANDER_REG_CONFIG;
        if (i2c_master_transmit_receive(dev, &reg, 1, &dir, 1, 50) == ESP_OK) {
            const uint8_t hi[2] = {EXPANDER_REG_OUTPUT,
                                   (uint8_t)(out | EXPANDER_SD_CS_BIT)};
            const uint8_t drv[2] = {EXPANDER_REG_CONFIG,
                                    (uint8_t)(dir & ~EXPANDER_SD_CS_BIT)};
            i2c_master_transmit(dev, hi, 2, 50);
            i2c_master_transmit(dev, drv, 2, 50);
        }
    }
    i2c_master_bus_rm_device(dev);
}

esp_err_t sdcard_mount(i2c_master_bus_handle_t bus)
{
    if (s_ready) {
        return ESP_OK;
    }
    release_card_cs(bus);

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;  // 20 MHz: kind to long traces
    host.flags &= ~SDMMC_HOST_FLAG_8BIT;
    host.flags &= ~SDMMC_HOST_FLAG_4BIT;
    host.flags |= SDMMC_HOST_FLAG_1BIT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = PIN_SD_CLK;
    slot.cmd = PIN_SD_CMD;
    slot.d0 = PIN_SD_D0;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,  // never reformat a user's card
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    const esp_err_t ret =
        esp_vfs_fat_sdmmc_mount(SDCARD_ROOT, &host, &slot, &mount, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "no card (%s); flash art and no logbook",
                 esp_err_to_name(ret));
        return ret;
    }

    s_ready = true;
    mkdir(SDCARD_DIR, 0777);
    uint64_t total = 0, freeb = 0;
    sdcard_usage(&total, &freeb);
    ESP_LOGI(TAG, "%s mounted: %llu MB free of %llu MB", s_card->cid.name,
             freeb / (1024 * 1024), total / (1024 * 1024));
    return ESP_OK;
}

bool sdcard_ready(void)
{
    return s_ready;
}

void sdcard_usage(uint64_t *total, uint64_t *freeb)
{
    *total = 0;
    *freeb = 0;
    if (!s_ready) {
        return;
    }
    uint64_t t = 0, f = 0;
    if (esp_vfs_fat_info(SDCARD_ROOT, &t, &f) == ESP_OK) {
        *total = t;
        *freeb = f;
    }
}
