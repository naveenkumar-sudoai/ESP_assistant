// =============================================================================
//  INMP441 I2S mic bring-up test.
//
//  Reads the mic and prints a live level meter so wiring and gain can be
//  verified in under a minute:
//    1. Wire the mic (see the README "INMP441" section).
//    2. Open this folder in PlatformIO and click Upload.
//    3. Open the Serial Plotter (and/or Serial Monitor) at 115200 baud.
//    4. Talk or tap the mic — the trace should jump when you make noise.
//
//  Output is three numbers per line, space-separated (plotted as 3 traces):
//      peak   rms   level(0-100)
//  - "peak" and "rms" are 16-bit sample magnitudes (0..32767).
//  - "level" is rms mapped to 0..100 (reference: ~4000 rms = 100%).
//
//  What to look for:
//    - Dead flat 0          -> silent: check L/R->GND, SD wiring, 3.3V power.
//    - Random/full-scale    -> wrong slot width or shift: adjust I2S_PCM_SHIFT.
//    - Pinned at max        -> clipping: lower I2S_GAIN_PERCENT or raise shift.
//    - Choppy/irregular     -> Wi-Fi interference / DMA size (not applicable here).
// =============================================================================

#include <Arduino.h>
#include <math.h>

#include "../../../src/mic_i2s.h"   // shares config.h + conversion with the main firmware

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("INMP441 I2S mic test");
  Serial.println("Open Tools > Serial Plotter (or Serial Monitor) at 115200 baud.");
  if (!mic_i2s_init()) {
    Serial.println("I2S init FAILED - check wiring: SCK=14, WS=27, SD=13, VDD=3.3V, L/R=GND.");
    while (1) { delay(1000); }
  }
  Serial.println("Ready. Columns: peak | rms | level(0-100)");
}

void loop() {
  int32_t raw[I2S_READ_SAMPLES];
  size_t bytesRead = 0;
  if (i2s_read(I2S_NUM_0, raw, sizeof(raw), &bytesRead, pdMS_TO_TICKS(100)) != ESP_OK ||
      bytesRead == 0) {
    return;
  }

  int n = bytesRead / 4;
  int64_t peak = 0;
  int64_t sumSq = 0;
  for (int i = 0; i < n; i++) {
    int16_t s = mic_i2s_convert(raw[i]);
    int a = abs((int)s);
    if (a > peak) peak = a;
    sumSq += (int64_t)s * (int64_t)s;
  }
  int rms = (n > 0) ? (int)sqrt((double)sumSq / n) : 0;
  int level = (int)constrain((rms * 100L) / 4000, 0, 100);   // 4000 rms ~ -18 dBFS -> 100%

  Serial.printf("%d %d %d\n", (int)peak, rms, level);
  delay(10);
}
