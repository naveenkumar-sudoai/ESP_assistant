#ifndef MIC_I2S_H
#define MIC_I2S_H

// =============================================================================
//  INMP441 I2S MEMS microphone — shared helper.
//
//  Used by BOTH the main firmware and the tests/i2s_mic_test bring-up sketch so
//  the pin map, sample rate, shift and gain always agree (they live in config.h).
//
//  Wiring (3.3V ONLY, never 5V):
//    VDD -> 3.3V      GND -> GND
//    SCK -> PIN_I2S_BCK   (bit clock; ESP32 OUTPUT)
//    WS  -> PIN_I2S_WS    (word select / LRCL; ESP32 OUTPUT)
//    SD  -> PIN_I2S_SD    (serial data; ESP32 INPUT)
//    L/R -> GND           (selects the LEFT channel — we read ONLY_LEFT)
//
//  Uses the ESP-IDF *legacy* I2S driver (driver/i2s.h), which compiles on both
//  Arduino-ESP32 core 2.x (IDF 4.4, the PlatformIO default) and core 3.x (IDF 5.x,
//  where it is kept as a deprecated-but-working shim). Chosen for reliability and
//  because it matches the default, unpinned "espressif32" platform.
// =============================================================================

#include <Arduino.h>
#include <driver/i2s.h>

#include "config.h"

// Initialize the I2S peripheral as master, RX, 16 kHz, mono (left channel),
// 32-bit slots (the INMP441 puts its 24-bit data inside a 32-bit slot).
// Returns false (and prints the reason) if the driver/pins fail to set up.
static inline bool mic_i2s_init() {
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,   // 24-bit data in a 32-bit slot
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,    // L/R->GND => left channel only
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 16,
        .dma_buf_len = 512,                              // frames per DMA buffer (~512 ms @16 kHz)
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0,
    };
    i2s_pin_config_t pins = {
        .bck_io_num = PIN_I2S_BCK,
        .ws_io_num = PIN_I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,               // RX only
        .data_in_num = PIN_I2S_SD,
    };

    if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) {
        Serial.println("[i2s] driver install failed");
        return false;
    }
    if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
        Serial.println("[i2s] set pin failed");
        i2s_driver_uninstall(I2S_NUM_0);
        return false;
    }
    Serial.printf("[i2s] INMP441 ready: %d Hz, 32-bit slots, left channel\n", I2S_SAMPLE_RATE);
    return true;
}

// Convert one raw 32-bit slot into a signed 16-bit PCM sample.
//
// With I2S_COMM_FORMAT_STAND_I2S the ESP32 absorbs the 1-bit I2S delay, so the
// INMP441's 24-bit signed data comes back LEFT-justified in the 32-bit slot
// (bits [31:8]). A right shift keeps the top 16 bits. I2S_PCM_SHIFT sets gain:
//   16 = unity (24-bit full-scale -> 16-bit full-scale)  [default]
//   15 = +6 dB
//   14 = +12 dB (may clip — the guard below limits it)
// Tune with tests/i2s_mic_test if the audio is too loud (clipping) or too quiet.
static inline int16_t mic_i2s_convert(int32_t raw) {
    static float dcPrevX = 0.0f;   // DC-blocker state (continuous across chunks)
    static float dcPrevY = 0.0f;

    int32_t v = raw >> I2S_PCM_SHIFT;                 // 24-bit -> 16-bit (tunable)
    v = (v * (int32_t)I2S_GAIN_PERCENT) / 100;        // optional extra gain

    if (I2S_REMOVE_DC) {
        // First-order DC blocker / high-pass: y[n] = x[n] - x[n-1] + R*y[n-1].
        float y = (float)v - dcPrevX + I2S_DC_COEF * dcPrevY;
        dcPrevX = (float)v;
        dcPrevY = y;
        v = (int32_t)y;
    }

    if (v > 32767)  v = 32767;                        // clip guard
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

#endif
