#include "display.h"

#include "config.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LCD_HOST SPI2_HOST
#define LCD_CS GPIO_NUM_12
#define LCD_PCLK GPIO_NUM_11
#define LCD_DATA0 GPIO_NUM_4
#define LCD_DATA1 GPIO_NUM_5
#define LCD_DATA2 GPIO_NUM_6
#define LCD_DATA3 GPIO_NUM_7

#define TOUCH_ADDR_CST820 0x15
#define V2_PANEL_X_GAP 0x10

#define LCD_CMD_BRIGHTNESS 0x51

// Drop to 40 MHz if the panel ever shows tearing or corrupted pixels; these
// signals go through the GPIO matrix rather than dedicated IOMUX pins.
#define LCD_PIXEL_CLOCK_HZ (80 * 1000 * 1000)

#define BAND_PIXELS (LCD_H_RES * BAND_ROWS)

static const char *TAG = "display";

// Two buffers, alternating by band index, so one can be filled while the other
// is being transmitted. DMA_ATTR keeps them in DMA-capable internal SRAM.
static DMA_ATTR uint16_t s_band_buf[2][BAND_PIXELS];

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_buffers_free;
static bool s_is_v2;

// Same power-on sequence Waveshare uses: pixel format RGB565, brightness and
// HBM registers open, full-window address set, sleep out, display on.
static const co5300_lcd_init_cmd_t s_init_cmds[] = {
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x11, NULL, 0, 100},
    {0x29, NULL, 0, 0},
};

static bool IRAM_ATTR on_trans_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *event,
                                    void *user_ctx)
{
    (void)io;
    (void)event;
    (void)user_ctx;

    BaseType_t higher_priority_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_buffers_free, &higher_priority_woken);
    return higher_priority_woken == pdTRUE;
}

// ---------------------------------------------------------------------------
// Hardware reset via the TCA9554 IO expander.
//
// The panel's reset line is NOT wired to an ESP32 pin: it sits on the board's
// IO expander, shared with the touch controller's reset (plus SD on another
// pin). A chip reset therefore never resets the panel — and an esptool reflash
// interrupts the panel mid-QSPI-transfer, which can wedge its controller in a
// state the software-only init cannot recover. Every command then reports OK
// into a black screen.
//
// Waveshare's own examples fix this by pulsing expander pins 0, 1, 2 and 6
// low for 20 ms before display init on every boot. Do exactly that, preserving
// the other pins (bit 4 is the PWR button input).
// ---------------------------------------------------------------------------

#define EXPANDER_ADDR 0x20
#define EXPANDER_REG_OUTPUT 0x01
#define EXPANDER_REG_CONFIG 0x03
#define EXPANDER_RESET_BITS 0x47  // pins 0, 1, 2, 6

static esp_err_t expander_hard_reset(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = EXPANDER_ADDR,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t dev;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &dev), TAG, "expander add");

    esp_err_t ret = ESP_OK;
    uint8_t out = 0xFF, dir = 0xFF;

    // Read-modify-write both registers so untouched pins keep their state.
    uint8_t reg = EXPANDER_REG_OUTPUT;
    if (i2c_master_transmit_receive(dev, &reg, 1, &out, 1, 50) != ESP_OK) goto fail;
    reg = EXPANDER_REG_CONFIG;
    if (i2c_master_transmit_receive(dev, &reg, 1, &dir, 1, 50) != ESP_OK) goto fail;

    {
        // Assert: output bits low first, then switch the pins to output.
        // Pulse TWICE with a generous hold: a single 20 ms pulse was observed
        // (2026-08-14) to leave a deeply wedged panel dark until a second
        // boot; two 50 ms pulses clear it in one.
        const uint8_t low[2] = {EXPANDER_REG_OUTPUT, (uint8_t)(out & ~EXPANDER_RESET_BITS)};
        const uint8_t drv[2] = {EXPANDER_REG_CONFIG, (uint8_t)(dir & ~EXPANDER_RESET_BITS)};
        const uint8_t high[2] = {EXPANDER_REG_OUTPUT, (uint8_t)(out | EXPANDER_RESET_BITS)};
        if (i2c_master_transmit(dev, low, 2, 50) != ESP_OK) goto fail;
        if (i2c_master_transmit(dev, drv, 2, 50) != ESP_OK) goto fail;
        for (int pulse = 0; pulse < 2; pulse++) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (i2c_master_transmit(dev, high, 2, 50) != ESP_OK) goto fail;
            vTaskDelay(pdMS_TO_TICKS(50));
            if (pulse == 0 &&
                i2c_master_transmit(dev, low, 2, 50) != ESP_OK) goto fail;
        }
    }

    // Panel and touch both need a beat after a hardware reset before they
    // accept commands.
    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_LOGI(TAG, "panel/touch hardware reset via IO expander done");
    goto out;

fail:
    ret = ESP_FAIL;
    ESP_LOGW(TAG, "IO expander reset failed; continuing with soft init only");
out:
    i2c_master_bus_rm_device(dev);
    return ret;
}

// The two board revisions differ only in the touch controller address and a
// 16 pixel horizontal offset on the panel.
static bool detect_v2(i2c_master_bus_handle_t bus)
{
    if (bus == NULL) {
        return false;
    }
    const bool is_v2 = i2c_master_probe(bus, TOUCH_ADDR_CST820, 50) == ESP_OK;
    ESP_LOGI(TAG, "detected %s board revision",
             is_v2 ? "V2 (CO5300/CST820)" : "original (SH8601/FT3168)");
    return is_v2;
}

esp_err_t display_init(i2c_master_bus_handle_t i2c_bus)
{
    // Hardware-reset the panel (and touch) before anything talks to them, so
    // every boot starts from a clean controller state no matter how the
    // previous session ended. See expander_hard_reset() for why.
    expander_hard_reset(i2c_bus);

    s_is_v2 = detect_v2(i2c_bus);

    s_buffers_free = xSemaphoreCreateCounting(2, 2);
    if (s_buffers_free == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const spi_bus_config_t bus_config = CO5300_PANEL_BUS_QSPI_CONFIG(
        LCD_PCLK, LCD_DATA0, LCD_DATA1, LCD_DATA2, LCD_DATA3,
        BAND_PIXELS * sizeof(uint16_t));
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO),
                        TAG, "spi bus init failed");

    esp_lcd_panel_io_spi_config_t io_config =
        CO5300_PANEL_IO_QSPI_CONFIG(LCD_CS, on_trans_done, NULL);
    // The driver's default is 40 MHz, which caps the panel at about 52 fps
    // since a full frame is 322 KB. 80 MHz is the ESP32-S3 SPI ceiling and
    // halves the transfer time.
    io_config.pclk_hz = LCD_PIXEL_CLOCK_HZ;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &s_io),
        TAG, "panel io init failed");

    const co5300_vendor_config_t vendor_config = {
        .init_cmds = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(s_io, &panel_config, &s_panel),
                        TAG, "panel init failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel setup failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, s_is_v2 ? V2_PANEL_X_GAP : 0, 0),
                        TAG, "panel gap failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display on failed");

    return ESP_OK;
}

uint16_t *display_acquire_band(void)
{
    // Transfers complete in the order they were queued, so once a slot frees up
    // the next buffer in rotation is guaranteed idle. The rotation counts
    // acquisitions rather than band indices: the renderer leaves out bands that
    // are already black, and keying off the band index would then hand the same
    // buffer to two transfers still in flight.
    xSemaphoreTake(s_buffers_free, portMAX_DELAY);

    static unsigned next;
    return s_band_buf[next++ & 1];
}

esp_err_t display_flush_band(int band_index, const uint16_t *buffer)
{
    const int y0 = band_index * BAND_ROWS;
    return esp_lcd_panel_draw_bitmap(s_panel, 0, y0, LCD_H_RES, y0 + BAND_ROWS, buffer);
}

// Undo display_power_off() by replaying the whole init list, not just the
// sleep-out and display-on at the end of it.
//
// Sleep-in resets the panel's display-control registers, and the ones that
// matter are set *before* those two: 0x51 is the brightness (0xFF), 0x53
// enables brightness control at all, 0x3A is the pixel format, 0x2A/0x2B are
// the address window. Waking with only 0x11 and 0x29 gives a panel that is
// genuinely on and accepts every transfer without error — at zero brightness.
// The flush counter stays clean and the screen stays dark, which is a
// miserable thing to debug. Replaying the list keeps this correct by
// construction if the sequence ever changes.
esp_err_t display_power_on(void)
{
    for (size_t i = 0; i < sizeof(s_init_cmds) / sizeof(s_init_cmds[0]); i++) {
        const co5300_lcd_init_cmd_t *c = &s_init_cmds[i];
        const esp_err_t ret = esp_lcd_panel_io_tx_param(
            s_io, c->cmd, c->data, c->data_bytes);
        if (ret != ESP_OK) {
            return ret;
        }
        if (c->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }
    return ESP_OK;
}

esp_err_t display_power_off(void)
{
    // Wait for both band buffers — i.e. all in-flight DMA — before touching
    // the command interface, or the panel wedges mid-transfer.
    xSemaphoreTake(s_buffers_free, portMAX_DELAY);
    xSemaphoreTake(s_buffers_free, portMAX_DELAY);
    xSemaphoreGive(s_buffers_free);
    xSemaphoreGive(s_buffers_free);

    // Display off, then sleep-in: the panel drops to microamps.
    esp_err_t ret = esp_lcd_panel_io_tx_param(s_io, 0x28, NULL, 0);
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_io_tx_param(s_io, 0x10, NULL, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    return ret;
}

esp_err_t display_set_brightness(uint8_t level)
{
    return esp_lcd_panel_io_tx_param(s_io, LCD_CMD_BRIGHTNESS, &level, 1);
}
