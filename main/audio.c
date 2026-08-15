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
static i2s_chan_handle_t s_rx;

// Mic detection results, read by the cat loop.
static volatile bool s_sound_heard;
static volatile float s_mic_rms;
static volatile float s_mic_ambient;

// Written by the cat loop, read by the synth task. A torn read of a float is
// impossible on Xtensa (aligned 32-bit stores are atomic), so no lock.
static volatile float s_target;
static volatile bool s_chirp_pending;
static volatile bool s_hiss_pending;
static volatile bool s_step_pending;
static volatile bool s_boing_pending;
static volatile bool s_slurp_pending;
static volatile bool s_swipe_pending;
static volatile int s_dash_pending;  // 0 none, else direction
static volatile int s_meow_pending;  // 0 none, else variant+1
static volatile int s_beep_pending;  // 0 none, else scale step+1
static volatile bool s_stop;

static esp_err_t i2s_init(const audio_codec_data_if_t **data_if)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    // Full duplex: the mic (codec ASDOUT -> DIN) shares the clock pair.
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx), TAG, "i2s channel");

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = PIN_I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s std tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "i2s std rx");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .tx_handle = s_tx,
        .rx_handle = s_rx,
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
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = PIN_POWER_AMP,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "es8311");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
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
    if (esp_codec_dev_set_in_gain(s_speaker, MIC_GAIN_DB) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "mic gain not set; detection may be deaf");
    }
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
    float step_env;   // footstep tap envelope
    float step_lpf;
    bool step_alt;    // alternate paw timbre
    float boing_t;    // seconds into the boing, or -1
    float boing_ph;
    float slurp_t;    // seconds into the slurp, or -1
    float slurp_lpf;
    float swipe_t;    // seconds into the swipe, or -1
    float swipe_lpf;
    float dash_t;     // seconds into the dash, or -1
    float dash_ph;
    float dash_lpf;
    int dash_dir;
    float meow_t;     // seconds into the meow, or -1
    float meow_ph;
    int meow_var;     // which candidate voice
    float beep_t;     // seconds into the heart-drain beep, or -1
    float beep_ph;
    int beep_step;    // which rung of the descending scale
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
    if (s_step_pending) {
        s_step_pending = false;
        st->step_env = 1.0f;
        st->step_alt = !st->step_alt;
    }
    if (s_boing_pending) {
        s_boing_pending = false;
        st->boing_t = 0.0f;
        st->boing_ph = 0.0f;
    }
    if (s_slurp_pending) {
        s_slurp_pending = false;
        st->slurp_t = 0.0f;
    }
    if (s_swipe_pending) {
        s_swipe_pending = false;
        st->swipe_t = 0.0f;
    }
    if (s_dash_pending) {
        st->dash_dir = s_dash_pending;
        s_dash_pending = 0;
        st->dash_t = 0.0f;
        st->dash_ph = 0.0f;
    }
    if (s_meow_pending) {
        st->meow_var = s_meow_pending - 1;
        s_meow_pending = 0;
        st->meow_t = 0.0f;
        st->meow_ph = 0.0f;
    }
    if (s_beep_pending) {
        st->beep_step = s_beep_pending - 1;
        s_beep_pending = 0;
        st->beep_t = 0.0f;
        st->beep_ph = 0.0f;
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

        // Footstep: a 45 ms tap of soft low-passed noise; alternate paws get
        // slightly different brightness so a walk pitter-patters.
        if (st->step_env > 0.002f) {
            const float cutoff = st->step_alt ? 950.0f : 680.0f;
            const float kf = 1.0f - expf(-TWO_PI * cutoff * dt);
            st->step_lpf += kf * (frand() - st->step_lpf);
            s += st->step_env * st->step_env * st->step_lpf * 0.55f;
            st->step_env -= dt / 0.045f;
        }

        // Boing: a sine whose pitch springs — a fast drop with a decaying
        // wobble — under a soft 0.35 s envelope.
        if (st->boing_t >= 0.0f) {
            const float t = st->boing_t;
            const float dur = 0.35f;
            if (t < dur) {
                // Pitched for a tiny speaker: centred ~420 Hz with the
                // spring wobble, plus a second harmonic for body.
                const float f = 420.0f + 260.0f * expf(-t * 5.0f) * cosf(TWO_PI * 10.0f * t);
                st->boing_ph += TWO_PI * f * dt;
                const float attack = (t < 0.008f) ? (t / 0.008f) : 1.0f;
                const float env = 0.32f * attack * expf(-t * 7.0f);
                s += env * (sinf(st->boing_ph) + 0.35f * sinf(2.0f * st->boing_ph));
                st->boing_t += dt;
            } else {
                st->boing_t = -1.0f;
            }
        }

        // Slurp: noise through a low-pass whose cutoff sweeps up quickly,
        // under a 160 ms bell envelope. Reads as a small wet lick.
        if (st->slurp_t >= 0.0f) {
            const float t = st->slurp_t;
            const float dur = 0.16f;
            if (t < dur) {
                const float u = t / dur;
                const float cutoff = 280.0f + 1300.0f * u * u;
                const float kf = 1.0f - expf(-TWO_PI * cutoff * dt);
                st->slurp_lpf += kf * (frand() - st->slurp_lpf);
                const float bell = sinf(3.14159265f * u);
                s += 0.17f * bell * st->slurp_lpf * 2.2f;
                st->slurp_t += dt;
            } else {
                st->slurp_t = -1.0f;
            }
        }

        // Swipe: airy high-passed noise under a 120 ms bell — the whisper of
        // a paw cutting through air. High-pass comes from subtracting the
        // low-passed noise from the raw noise.
        if (st->swipe_t >= 0.0f) {
            const float t = st->swipe_t;
            const float dur = 0.12f;
            if (t < dur) {
                const float u = t / dur;
                const float n = frand();
                const float kf = 1.0f - expf(-TWO_PI * 900.0f * dt);
                st->swipe_lpf += kf * (n - st->swipe_lpf);
                const float bell = sinf(3.14159265f * u);
                s += 0.13f * bell * (n - st->swipe_lpf);
                st->swipe_t += dt;
            } else {
                st->swipe_t = -1.0f;
            }
        }

        // Dash: a quick whoosh of tilted noise plus a directional pitch
        // sweep — rising when bounding right, falling when bounding left.
        if (st->dash_t >= 0.0f) {
            const float t = st->dash_t;
            const float dur = 0.22f;
            if (t < dur) {
                const float u = t / dur;
                const float n = frand();
                const float kf = 1.0f - expf(-TWO_PI * 1400.0f * dt);
                st->dash_lpf += kf * (n - st->dash_lpf);
                const float bell = sinf(3.14159265f * u);
                const float f = (st->dash_dir > 0) ? (420.0f + 480.0f * u)
                                                   : (900.0f - 480.0f * u);
                st->dash_ph += TWO_PI * f * dt;
                s += bell * (0.16f * (n - st->dash_lpf) + 0.14f * sinf(st->dash_ph));
                st->dash_t += dt;
            } else {
                st->dash_t = -1.0f;
            }
        }

        // Meow: a fundamental that rises into the "ee" and settles through
        // the "ow", sung through a small harmonic stack whose brightness
        // opens and closes with the vowel. Three candidate voices to pick
        // from by ear.
        if (st->meow_t >= 0.0f) {
            // {duration, start Hz, peak Hz, end Hz, brightness}
            static const float V[3][5] = {
                {0.70f, 340.0f, 620.0f, 380.0f, 1.00f},  // classic mrraow
                {0.55f, 420.0f, 750.0f, 460.0f, 1.25f},  // brighter, kittenish
                {0.90f, 300.0f, 520.0f, 330.0f, 0.80f},  // low, plaintive
            };
            const float *v = V[st->meow_var % 3];
            const float t = st->meow_t;
            const float dur = v[0];
            if (t < dur) {
                const float u = t / dur;
                // Pitch: rise over the first 35%, ease down after.
                const float f =
                    (u < 0.35f)
                        ? v[1] + (v[2] - v[1]) * (u / 0.35f)
                        : v[2] + (v[3] - v[2]) * ((u - 0.35f) / 0.65f) *
                                     ((u - 0.35f) / 0.65f);
                const float vib = 1.0f + 0.010f * sinf(TWO_PI * 6.5f * t);
                st->meow_ph += TWO_PI * f * vib * dt;
                // Vowel: brightness peaks with the pitch, then closes.
                const float bright =
                    v[4] * ((u < 0.4f) ? (0.35f + 1.6f * u) : (0.99f - 0.9f * (u - 0.4f)));
                const float ph = st->meow_ph;
                float m = sinf(ph) + 0.55f * bright * sinf(2.0f * ph) +
                          0.30f * bright * sinf(3.0f * ph) +
                          0.15f * bright * bright * sinf(4.0f * ph);
                const float attack = (t < 0.06f) ? (t / 0.06f) : 1.0f;
                const float rel = (u > 0.75f) ? (1.0f - u) / 0.25f : 1.0f;
                s += 0.24f * attack * rel * m;
                st->meow_t += dt;
            } else {
                st->meow_t = -1.0f;
            }
        }

        // Heart-drain beep: a clean tone per gauge row lost, each rung of
        // the scale lower than the last — the sound of love draining out.
        if (st->beep_t >= 0.0f) {
            static const float k_rungs[6] = {880.0f, 740.0f, 622.0f,
                                             523.0f, 440.0f, 370.0f};
            const float t = st->beep_t;
            const float dur = 0.20f;
            if (t < dur) {
                const float u = t / dur;
                st->beep_ph += TWO_PI * k_rungs[st->beep_step % 6] * dt;
                // Quick attack, long fall — a soft electronic pip.
                const float env = (u < 0.06f) ? (u / 0.06f)
                                              : expf(-(u - 0.06f) * 5.0f);
                s += 0.30f * env * (sinf(st->beep_ph) +
                                    0.22f * sinf(2.0f * st->beep_ph));
                st->beep_t += dt;
            } else {
                st->beep_t = -1.0f;
            }
        }

        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        out[i] = (int16_t)(s * 30000.0f);
    }
}

static bool synth_audible(const purr_state_t *st)
{
    return st->level > 0.01f || st->chirp_t >= 0.0f || st->hiss_t >= 0.0f ||
           st->boing_t >= 0.0f || st->slurp_t >= 0.0f || st->swipe_t >= 0.0f ||
           st->dash_t >= 0.0f || st->step_env > 0.01f ||
           st->meow_t >= 0.0f || st->beep_t >= 0.0f;
}

// ---------------------------------------------------------------------------
// The ear: each mic frame's RMS is compared against a slow ambient floor.
// Any sharp sound clears it — a psst, a snap, a knock. Detection is gated
// while (and shortly after) his own speaker sounds, so he cannot summon
// himself.
// ---------------------------------------------------------------------------

static void listen_frame(const int16_t *in, bool gated)
{
    float sum = 0.0f;
    for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
        const float v = (float)in[i] * (1.0f / 32768.0f);
        sum += v * v;
    }
    const float rms = sqrtf(sum / AUDIO_FRAME_SAMPLES);
    s_mic_rms = rms;

    static float ambient = 0.004f;
    const float frame_s = (float)AUDIO_FRAME_SAMPLES / AUDIO_SAMPLE_RATE;

    if (!gated) {
        const float floor_ = (ambient > 0.002f) ? ambient : 0.002f;
        if (rms > floor_ * MIC_TRIGGER_RATIO && rms > floor_ + MIC_TRIGGER_MIN) {
            s_sound_heard = true;
        }
    }
    // The floor tracks quiet frames quickly and loud ones very slowly, so a
    // burst of talking does not teach him to ignore you.
    const float tau = (rms < ambient) ? MIC_AMBIENT_TAU_S : MIC_AMBIENT_TAU_S * 6.0f;
    ambient += (rms - ambient) * (frame_s / tau);
    s_mic_ambient = ambient;
}

static void purr_task(void *arg)
{
    (void)arg;
    static int16_t frame[AUDIO_FRAME_SAMPLES];
    static int16_t mic[AUDIO_FRAME_SAMPLES];
    purr_state_t st = {.chirp_t = -1.0f, .hiss_t = -1.0f, .boing_t = -1.0f, .slurp_t = -1.0f, .swipe_t = -1.0f, .dash_t = -1.0f, .meow_t = -1.0f, .beep_t = -1.0f};
    float gate_hold = 0.0f;
    const float frame_s = (float)AUDIO_FRAME_SAMPLES / AUDIO_SAMPLE_RATE;

    for (;;) {
        if (s_stop) {
            vTaskDelete(NULL);
        }
        fill_frame(&st, frame);
        const int ret = esp_codec_dev_write(s_speaker, frame, sizeof(frame));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "speaker write failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // Same rate, same frame size: one mic frame per speaker frame keeps
        // the duplex loop self-pacing.
        if (esp_codec_dev_read(s_speaker, mic, sizeof(mic)) == ESP_CODEC_DEV_OK) {
            gate_hold = synth_audible(&st) ? MIC_GATE_HOLD_S
                                           : fmaxf(0.0f, gate_hold - frame_s);
            listen_frame(mic, gate_hold > 0.0f);
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

void audio_step(void)
{
    s_step_pending = true;
}

void audio_boing(void)
{
    s_boing_pending = true;
}

void audio_slurp(void)
{
    s_slurp_pending = true;
}

void audio_swipe(void)
{
    s_swipe_pending = true;
}

void audio_dash(int dir)
{
    s_dash_pending = (dir < 0) ? -1 : 1;
}

void audio_meow(int variant)
{
    s_meow_pending = (variant % 3) + 1;
}

void audio_beep(int step)
{
    s_beep_pending = ((step < 0) ? 0 : step % 6) + 1;
}

bool audio_take_sound(void)
{
    const bool v = s_sound_heard;
    s_sound_heard = false;
    return v;
}

void audio_mic_levels(float *rms, float *ambient)
{
    *rms = s_mic_rms;
    *ambient = s_mic_ambient;
}

void audio_stop(void)
{
    s_stop = true;
    vTaskDelay(pdMS_TO_TICKS(50));  // let the synth task exit
    if (s_speaker) {
        esp_codec_dev_close(s_speaker);  // also drops the PA enable
    }
}
