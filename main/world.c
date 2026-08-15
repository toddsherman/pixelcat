#include "config.h"
#if WORLD_FROM_SD

// The park.
//
// Each variant is BG_WORLD_W pre-rotated column strips of BG_STRIP_H pixels
// — 1.75 MB apiece, far too much to keep five of in flash uncompressed. But
// the art is 8-pixel blocky, so it DEFLATEs about 75:1: all five variants
// ride along in 127 KB and inflate into PSRAM through the ROM's tinfl,
// costing nothing but a fraction of a second.
//
// Two worlds live in PSRAM: the one being drawn and a spare the loader
// fills in the background, so a daypart change never stalls a frame. A
// matching world_N.bin on the SD card overrides the built-in one, which is
// how new art gets tried without a reflash.

#include <stdio.h>
#include <string.h>

#include "cat_bg.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdcard.h"
#include "world.h"

static const char *TAG = "world";

typedef uint16_t strip_t[BG_STRIP_H];

#define WORLD_BYTES ((size_t)BG_WORLD_W * BG_STRIP_H * sizeof(uint16_t))

static strip_t *s_buf[2];
static int s_variant[2] = {-1, -1};
static volatile int s_active;      // buffer the renderer is reading
static volatile int s_want = -1;   // variant the game has asked for
static QueueHandle_t s_requests;

#define SWAP16(v) ((uint16_t)((((v) >> 8) & 0xFF) | (((v) & 0xFF) << 8)))

// The fallback when there is no card: a flat sky over a flat ground, in the
// panel's byte order. Not the park, but not a black screen either.
static void paint_plain(strip_t *dst, int variant)
{
    // Sky and ground per daypart, roughly matching the real art's mood.
    static const uint8_t sky[BG_VARIANTS][3] = {
        {126, 192, 238}, {150, 140, 170}, {196, 120, 120},
        {70, 66, 110}, {24, 26, 54},
    };
    static const uint8_t gnd[BG_VARIANTS][3] = {
        {96, 150, 82}, {74, 96, 78}, {92, 74, 66},
        {40, 44, 46}, {18, 22, 26},
    };
    const int v = (variant >= 0 && variant < BG_VARIANTS) ? variant : 0;
    const uint16_t s565 = SWAP16((uint16_t)(((sky[v][0] & 0xF8) << 8) |
                                            ((sky[v][1] & 0xFC) << 3) |
                                            (sky[v][2] >> 3)));
    const uint16_t g565 = SWAP16((uint16_t)(((gnd[v][0] & 0xF8) << 8) |
                                            ((gnd[v][1] & 0xFC) << 3) |
                                            (gnd[v][2] >> 3)));
    // A strip is one world column: logical y runs from BG_STRIP_H-1 down to
    // 0, so the ground occupies the low indices.
    for (int x = 0; x < BG_WORLD_W; x++) {
        for (int y = 0; y < BG_STRIP_H; y++) {
            dst[x][y] = (y < 90) ? g565 : s565;
        }
    }
}

static bool load_variant(strip_t *dst, int variant)
{
    if (!sdcard_ready()) {
        return false;
    }
    char path[64];
    snprintf(path, sizeof(path), SDCARD_DIR "/world_%d.bin", variant);
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "%s missing", path);
        return false;
    }
    // Read in chunks so a truncated file fails cleanly rather than half-way.
    size_t got = 0;
    uint8_t *p = (uint8_t *)dst;
    while (got < WORLD_BYTES) {
        const size_t n = fread(p + got, 1, 32 * 1024, f);
        if (n == 0) {
            break;
        }
        got += n;
    }
    fclose(f);
    if (got != WORLD_BYTES) {
        ESP_LOGW(TAG, "%s is %u bytes, expected %u", path, (unsigned)got,
                 (unsigned)WORLD_BYTES);
        return false;
    }
    return true;
}

// Raw DEFLATE out of the ESP32-S3 ROM. Declared here because IDF ships no
// header for the ROM's miniz on this target.
#define TINFL_FAILED ((size_t)-1)
size_t tinfl_decompress_mem_to_mem(void *out, size_t out_len, const void *src,
                                   size_t src_len, int flags);

static bool inflate_variant(strip_t *dst, int variant)
{
    const size_t got = tinfl_decompress_mem_to_mem(
        dst, WORLD_BYTES, cat_bg_z[variant], cat_bg_z_len[variant], 0);
    if (got != WORLD_BYTES) {
        ESP_LOGW(TAG, "variant %d inflated to %u, expected %u", variant,
                 (unsigned)got, (unsigned)WORLD_BYTES);
        return false;
    }
    return true;
}

static void fill(int slot, int variant)
{
    // The card wins if it has this variant — that is how new art is tried
    // without a reflash — otherwise the built-in copy inflates.
    if (!load_variant(s_buf[slot], variant) &&
        !inflate_variant(s_buf[slot], variant)) {
        paint_plain(s_buf[slot], variant);
    }
    s_variant[slot] = variant;
}

static void loader_task(void *arg)
{
    (void)arg;
    for (;;) {
        int variant;
        if (xQueueReceive(s_requests, &variant, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (variant == s_variant[s_active]) {
            continue;  // already on screen
        }
        const int spare = s_active ^ 1;
        if (variant != s_variant[spare]) {
            fill(spare, variant);
        }
        s_active = spare;  // single word: the renderer sees old or new, never torn
        ESP_LOGI(TAG, "variant %d resident (slot %d)", variant, spare);
    }
}

esp_err_t world_init(int variant)
{
    for (int i = 0; i < 2; i++) {
        s_buf[i] = heap_caps_malloc(WORLD_BYTES, MALLOC_CAP_SPIRAM);
        if (!s_buf[i]) {
            ESP_LOGE(TAG, "no PSRAM for the park (%u bytes)",
                     (unsigned)WORLD_BYTES);
            return ESP_ERR_NO_MEM;
        }
    }
    // Something safe to draw immediately, so the first frames have a sky
    // rather than uninitialised PSRAM.
    paint_plain(s_buf[0], variant);
    s_variant[0] = -1;
    s_active = 0;

    // The real park inflates on the loader's stack, never the caller's:
    // miniz keeps an ~11 KB decompressor as a local, which is more than
    // app_main has to spare.
    s_requests = xQueueCreate(4, sizeof(int));
    if (!s_requests ||
        xTaskCreatePinnedToCore(loader_task, "world", 16384, NULL, 3, NULL,
                                0) != pdPASS) {
        ESP_LOGW(TAG, "no loader task; the park will not change with the sun");
        return ESP_FAIL;
    }
    world_request(variant);
    return ESP_OK;
}

bool world_is_resident(int variant)
{
    return s_variant[s_active] == variant;
}

void world_request(int variant)
{
    if (variant < 0 || variant >= BG_VARIANTS || variant == s_want) {
        return;
    }
    s_want = variant;
    if (s_requests) {
        xQueueSend(s_requests, &variant, 0);
    }
}

const uint16_t (*cat_bg_strips(int variant))[BG_STRIP_H]
{
    world_request(variant);
    return s_buf[s_active] ? (const uint16_t (*)[BG_STRIP_H])s_buf[s_active]
                           : NULL;
}

#endif  // WORLD_FROM_SD
