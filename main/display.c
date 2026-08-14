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

esp_err_t display_set_brightness(uint8_t level)
{
    return esp_lcd_panel_io_tx_param(s_io, LCD_CMD_BRIGHTNESS, &level, 1);
}
