#pragma once

// Every tunable in PixelCat lives here.

// ---------------------------------------------------------------------------
// Display (same panel driver as fluidbox: CO5300/SH8601 over QSPI, band DMA)
// ---------------------------------------------------------------------------

#define LCD_H_RES 368
#define LCD_V_RES 448

#define BAND_ROWS 28
#define BAND_COUNT (LCD_V_RES / BAND_ROWS)

#define DISPLAY_BRIGHTNESS 210  // 0..255, written to panel register 0x51

// ---------------------------------------------------------------------------
// Pixel art canvas
// ---------------------------------------------------------------------------

// 1 = the wandering sprite-sheet cat (16px poses: walks, grooms, loafs).
// 0 = the original big kawaii cat.
#define CAT_SPRITE_STYLE 1

// The device is held rotated 90 degrees counter-clockwise: the panel scans
// portrait 368x448, but everything logical is landscape 448x368. Rotation is
// done in software — backgrounds are baked pre-rotated, the sprite overlay
// maps through the rotation at render time.
#define ROTATE_CCW 1

// The cat is composed on a small indexed-colour canvas and scaled up by an
// integer factor to the panel. The sprite cat runs chunkier (8x) than the
// big cat (4x).
#if CAT_SPRITE_STYLE
#define PIX_SCALE 8
#else
#define PIX_SCALE 4
#endif

// Logical (viewer) dimensions.
#if ROTATE_CCW
#define VIEW_W LCD_V_RES
#define VIEW_H LCD_H_RES
#else
#define VIEW_W LCD_H_RES
#define VIEW_H LCD_V_RES
#endif
#define CANVAS_W (VIEW_W / PIX_SCALE)
#define CANVAS_H (VIEW_H / PIX_SCALE)

// Frame pacing for animation + touch polling.
#define CAT_FPS 30

// ---------------------------------------------------------------------------
// Behaviour
// ---------------------------------------------------------------------------

// A press that moves less than this many logical pixels is a poke, not a pet.
#define PET_MOVE_MIN_PX 4

// Stroke speed (logical px/s) that counts as maximum-happiness petting.
#define PET_SPEED_FULL 90.0f

// How quickly purr intensity chases the petting level, per second.
#define PET_RAMP_UP 2.5f
#define PET_RAMP_DOWN 0.6f

// Seconds without interaction before the cat falls asleep.
#define SLEEP_AFTER_S 60.0f

// Seconds the startled reaction to a poke lasts.
#define STARTLE_S 0.8f

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_FRAME_SAMPLES 256

// Speaker volume, 0..100 (esp_codec_dev scale).
#define PURR_VOLUME 78

// A real cat purrs at 20-30 Hz on both the in- and out-breath. The synth
// amplitude-modulates low-passed noise at this rate.
#define PURR_RATE_HZ 24.0f

// Breath cycle that swells and softens the purr, Hz.
#define PURR_BREATH_HZ 0.45f

// One-pole low-pass cutoff for the rumble noise, Hz.
#define PURR_NOISE_CUTOFF_HZ 260.0f

// Master purr loudness, 0..1, applied before the int16 conversion.
#define PURR_LEVEL 0.85f

// Little rising "mrrp?" chirp when the cat wakes or is first petted.
#define CHIRP_ENABLE 1

// ---------------------------------------------------------------------------
// I2C / board wiring (from the Waveshare BSP for this board)
// ---------------------------------------------------------------------------

#define PIN_I2C_SDA 15
#define PIN_I2C_SCL 14

#define PIN_I2S_MCLK 16
#define PIN_I2S_BCLK 9
#define PIN_I2S_WS 45
#define PIN_I2S_DOUT 8

// Speaker power amplifier enable.
#define PIN_POWER_AMP 46
