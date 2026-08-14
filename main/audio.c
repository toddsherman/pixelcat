#include "audio.h"

#include <math.h>

#include "config.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TWO_PI 6.28318530717958647692f

static const char *TAG = "audio";

static esp_codec_dev_handle_t s_speaker;
static i2s_chan_handle_t s_tx;

// Written by the cat loop, read by the synth task. A torn read of a float is
// impossible on Xtensa (aligned 32-bit stores are atomic), so no lock.
static volatile float s_target;
static volatile bool s_chirp_pending;
static volatile bool s_hiss_pending;

static esp_err_t i2s_init(const audio_codec_data_if_t **data_if)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s channel");

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s std");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .tx_handle = s_tx,
        .rx_handle = NULL,
    };
    *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(*data_if, ESP_FAIL, TAG, "i2s data interface");
    return ESP_OK;
}

static esp_err_t codec_init(i2c_master_bus_handle_t bus, const audio_codec_data_if_t *data_if)
{
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if, ESP_FAIL, TAG, "gpio interface");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = 0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "i2c control interface");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = PIN_POWER_AMP,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "es8311");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_speaker = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_speaker, ESP_FAIL, TAG, "codec dev");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = AUDIO_SAMPLE_RATE,
        .mclk_multiple = 256,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_speaker, &fs) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "codec open");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_out_vol(s_speaker, PURR_VOLUME) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "codec volume");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// The purr.
//
// Recipe: low-passed white noise (the rumble) amplitude-modulated at
// PURR_RATE_HZ (the flutter every real purr has), the whole thing swelling and
// easing with a slow breath cycle. A touch of 2x-rate sine underneath gives it
// body on a small speaker that cannot reproduce 24 Hz directly.
// ---------------------------------------------------------------------------

static uint32_t s_rng = 0x2545F491;

static inline float frand(void)
{
    // xorshift, mapped to -1..1.
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(int32_t)s_rng * (1.0f / 2147483648.0f);
}

typedef struct {
    float level;      // smoothed intensity actually sounding
    float purr_ph;    // purr flutter phase
    float breath_ph;  // breath cycle phase
    float body_ph;    // low sine body phase
    float lpf;        // one-pole noise filter state
    float chirp_t;    // seconds into the chirp, or -1 when idle
    float hiss_t;     // seconds into the hiss, or -1 when idle
    float hiss_lpf;   // filter state for tilting the hiss noise
} purr_state_t;

static void fill_frame(purr_state_t *st, int16_t *out)
{
    const float dt = 1.0f / AUDIO_SAMPLE_RATE;
    const float lpf_k = 1.0f - expf(-TWO_PI * PURR_NOISE_CUTOFF_HZ * dt);

    // Chase the target slowly enough to be a cat, not a switch.
    const float target = s_target;
    const float frame_dt = (float)AUDIO_FRAME_SAMPLES * dt;
    const float rate = (target > st->level) ? PET_RAMP_UP : PET_RAMP_DOWN;
    float step = rate * frame_dt;
    if (fabsf(target - st->level) <= step) {
        st->level = target;
    } else {
        st->level += (target > st->level) ? step : -step;
    }

    if (s_chirp_pending) {
        s_chirp_pending = false;
        st->chirp_t = 0.0f;
    }
    if (s_hiss_pending) {
        s_hiss_pending = false;
        st->hiss_t = 0.0f;
    }

    for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
        float s = 0.0f;

        if (st->level > 0.003f) {
            st->lpf += lpf_k * (frand() - st->lpf);

            // Flutter: never fully closed, so the purr does not tick.
            const float flutter = 0.42f + 0.58f * (0.5f + 0.5f * sinf(st->purr_ph));
            // Breath: asymmetric swell, louder on the exhale half.
            const float br = sinf(st->breath_ph);
            const float breath = 0.62f + 0.30f * br + 0.08f * sinf(2.0f * st->breath_ph);

            const float rumble = st->lpf * 2.6f + 0.30f * sinf(st->body_ph);
            s = st->level * PURR_LEVEL * breath * flutter * rumble;

            st->purr_ph += TWO_PI * PURR_RATE_HZ * dt;
            st->body_ph += TWO_PI * (PURR_RATE_HZ * 2.0f) * dt;
            st->breath_ph += TWO_PI * PURR_BREATH_HZ * dt;
            if (st->purr_ph > TWO_PI) st->purr_ph -= TWO_PI;
            if (st->body_ph > TWO_PI) st->body_ph -= TWO_PI;
            if (st->breath_ph > TWO_PI) st->breath_ph -= TWO_PI;
        }

#if CHIRP_ENABLE
        if (st->chirp_t >= 0.0f) {
            // "mrrp?": 250 ms sine sweep rising 550->1050 Hz with vibrato and
            // a smooth attack/decay envelope.
            const float t = st->chirp_t;
            const float dur = 0.25f;
            if (t < dur) {
                const float u = t / dur;
                const float f = 550.0f + 500.0f * u * u + 40.0f * sinf(TWO_PI * 28.0f * t);
                const float env = sinf(3.14159265f * u);
                s += 0.30f * env * env * sinf(TWO_PI * f * t);
                st->chirp_t += dt;
            } else {
                st->chirp_t = -1.0f;
            }
        }
#endif

        if (st->hiss_t >= 0.0f) {
            // A cat hiss is broadband noise with the low end rolled off:
            // subtracting a low-pass leaves the high-tilted remainder. The
            // envelope snaps on in 30 ms and dies away over ~0.8 s.
            const float t = st->hiss_t;
            const float dur = 0.85f;
            if (t < dur) {
                const float n = frand();
                st->hiss_lpf += 0.18f * (n - st->hiss_lpf);
                const float attack = (t < 0.03f) ? (t / 0.03f) : 1.0f;
                const float decay = 1.0f - (t / dur);
                s += 0.55f * attack * decay * decay * (n - st->hiss_lpf);
                st->hiss_t += dt;
            } else {
                st->hiss_t = -1.0f;
            }
        }

        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        out[i] = (int16_t)(s * 30000.0f);
    }
}

static void purr_task(void *arg)
{
    (void)arg;
    static int16_t frame[AUDIO_FRAME_SAMPLES];
    purr_state_t st = {.chirp_t = -1.0f, .hiss_t = -1.0f};

    for (;;) {
        fill_frame(&st, frame);
        const int ret = esp_codec_dev_write(s_speaker, frame, sizeof(frame));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "speaker write failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

esp_err_t audio_init(i2c_master_bus_handle_t i2c_bus)
{
    const audio_codec_data_if_t *data_if = NULL;
    ESP_RETURN_ON_ERROR(i2s_init(&data_if), TAG, "i2s");
    ESP_RETURN_ON_ERROR(codec_init(i2c_bus, data_if), TAG, "codec");

    xTaskCreatePinnedToCore(purr_task, "purr", 4096, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "purr synth running");
    return ESP_OK;
}

void audio_set_purr(float intensity)
{
    if (intensity < 0.0f) intensity = 0.0f;
    if (intensity > 1.0f) intensity = 1.0f;
    s_target = intensity;
}

void audio_chirp(void)
{
    s_chirp_pending = true;
}

void audio_hiss(void)
{
    s_hiss_pending = true;
}
