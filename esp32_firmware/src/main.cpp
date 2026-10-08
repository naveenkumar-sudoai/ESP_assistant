// =============================================================================
//  Whisper Assistant — ESP32 firmware ("the face")
//
//  Responsibilities:
//    - Connect to Wi-Fi and run a tiny HTTP server.
//    - Drive the SSD1306 OLED (status, responses, vertical auto-scroll).
//    - Read 3 buttons (Home / Mode / Hold-to-speak) with debouncing.
//    - Drive a status LED (steady while listening, blink while thinking,
//      flash on ambient sound when the loudness sensor is enabled).
//    - Talk to the laptop "brain" over HTTP:
//          ->  GET /start_listening?mode=<mode>    (hold button pressed)
//          ->  GET /stop_listening                  (hold button released)
//          <-  POST /display                        (laptop sends final text back)
//
//  Built with PlatformIO. Edit config.h before flashing — nothing else.
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>

#include "config.h"

#if AUDIO_SOURCE_ESP32
#include "mic_i2s.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#endif

// ------------------------------------------------------------------ display --
Adafruit_SSD1306 display(128, 64, &Wire, -1);   // -1 = no hardware reset pin

// --------------------------------------------------------------------- HTTP --
WebServer server(ESP_HTTP_PORT);

// -------------------------------------------------------------------- state --
enum AppState { STATE_BOOT, STATE_IDLE, STATE_LISTENING, STATE_THINKING, STATE_DISPLAY };
enum AppMode  { MODE_HOME, MODE_SUMMARIZE, MODE_TODO };

AppState state = STATE_BOOT;
AppMode  mode  = MODE_HOME;

// Response text received from the laptop, plus display/scroll animation state.
String  responseText        = "";
String  displayLines[32];
int     displayLineCount    = 0;
int     displayScrollMax    = 0;    // 0 => fits on screen (static, no scroll)
int     displayScrollOffset = 0;
enum    ScrollPhase { SCROLL_TOP, SCROLL_DOWN, SCROLL_BOTTOM };
ScrollPhase scrollPhase = SCROLL_TOP;

unsigned long displayStartedMs = 0;
unsigned long scrollTimerMs     = 0;
unsigned long thinkingStartedMs = 0;
unsigned long bootIpShownAt     = 0;
unsigned long redrawIdleAt      = 0;
unsigned long lastSoundDetectMs = 0;
unsigned long lastWifiCheck     = 0;

#if AUDIO_SOURCE_ESP32
// ------------------------------------------------------- on-device audio ----
// A capture task reads the INMP441 over I2S and pushes 16-bit PCM into a
// ring-buffer queue; a sender task drains it and POSTs small chunks to the
// laptop. Two tasks decouple hard-real-time capture from bursty Wi-Fi sends,
// so the OLED/button/web-server loop does not drop samples under normal
// conditions. (A network stall longer than the combined queue + DMA buffer —
// ~2.5 s — will still drop audio; that's inherent to streaming with no local
// disk. The backpressure keeps the drop bounded rather than unbounded.)
//
// The `g_streaming` flag is also the "busy" guard for any future eco/sleep
// mode: never enter sleep while it is true (and wake on any button).
void callLaptop(const String &path);   // forward decl (defined below)

typedef struct {
  uint8_t  data[I2S_CHUNK_SAMPLES * 2];
  uint16_t len;                        // bytes used; 0 = end-of-stream sentinel
} AudioBlock;

QueueHandle_t  audioQueue        = NULL;
TaskHandle_t   captureTaskHandle = NULL;
TaskHandle_t   senderTaskHandle  = NULL;

volatile bool  micReady          = false;
volatile bool  g_streaming       = false;   // a capture session is active
volatile bool  g_stopRequested   = false;   // release pressed or MAX_RECORD_MS hit
volatile bool  g_cancelSession   = false;   // tap too short -> discard, don't transcribe
volatile bool  g_maxReached      = false;   // capture auto-stopped on MAX_RECORD_MS

String   g_sessionId   = "";
uint32_t g_chunkIndex  = 0;
unsigned long pressStartMs = 0;

// POST one chunk of raw 16-bit PCM to the laptop's /audio_chunk endpoint.
void postAudioChunk(const String &sid, uint32_t idx, const uint8_t *data, size_t len) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[audio] WiFi down, dropping chunk");
    return;
  }
  HTTPClient http;
  http.setTimeout(1500);
  String url = String("http://") + LAPTOP_IP + ":" + String(LAPTOP_PORT) +
               "/audio_chunk?session=" + sid + "&index=" + String(idx);
  http.begin(url);
  http.addHeader("Content-Type", "application/octet-stream");
  int code = http.POST((uint8_t *)data, len);
  if (code != 200) Serial.printf("[audio] chunk %u -> %d\n", idx, code);
  http.end();
}

// Capture task: I2S -> convert -> ring buffer. Runs at high priority so it is
// never starved (which would drop DMA samples). Backpressures on a full queue.
void captureTask(void *arg) {
  int32_t raw[I2S_READ_SAMPLES];
  int16_t block[I2S_CHUNK_SAMPLES];
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);        // wait for "start" from loop()
    uint32_t startMs = millis();
    int blockLen = 0;                                // samples accumulated in `block`
    while (!g_stopRequested) {
      size_t bytesRead = 0;
      esp_err_t err = i2s_read(I2S_NUM_0, raw, sizeof(raw), &bytesRead, pdMS_TO_TICKS(100));
      if (err != ESP_OK || bytesRead == 0) continue; // timeout -> re-check stop flag
      int n = bytesRead / 4;
      for (int i = 0; i < n; i++) {
        block[blockLen++] = mic_i2s_convert(raw[i]);
        if (blockLen == I2S_CHUNK_SAMPLES) {
          AudioBlock b;
          b.len = blockLen * 2;
          memcpy(b.data, block, b.len);
          xQueueSend(audioQueue, &b, portMAX_DELAY); // backpressure if network is slow
          blockLen = 0;
        }
      }
      if (millis() - startMs >= MAX_RECORD_MS) {
        g_stopRequested = true;
        g_maxReached = true;
      }
    }
    if (blockLen > 0) {                              // flush the partial final block
      AudioBlock b;
      b.len = blockLen * 2;
      memcpy(b.data, block, b.len);
      xQueueSend(audioQueue, &b, portMAX_DELAY);
    }
    AudioBlock end;                                  // end-of-stream sentinel
    end.len = 0;
    xQueueSend(audioQueue, &end, portMAX_DELAY);
  }
}

// Sender task: drains the ring buffer and POSTs chunks; on the end sentinel it
// finishes the session by telling the laptop to stop (or cancel) transcription.
void senderTask(void *arg) {
  AudioBlock b;
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);        // wait for "start"
    while (true) {
      if (xQueueReceive(audioQueue, &b, portMAX_DELAY) != pdTRUE) continue;
      if (b.len == 0) break;                        // end of stream
      postAudioChunk(g_sessionId, g_chunkIndex++, b.data, b.len);
    }
    if (g_cancelSession) {
      callLaptop("/cancel?session=" + g_sessionId);
    } else {
      callLaptop("/stop_listening?session=" + g_sessionId);
    }
    g_streaming = false;                            // ready for the next utterance
  }
}
#endif

// ----------------------------------------------------------------- buttons --
struct Button {
  uint8_t pin;
  bool lastReading;
  bool stable;          // debounced state (HIGH = released)
  unsigned long lastChangeMs;
  bool pressed;         // latched falling edge
  bool released;        // latched rising edge
};
Button btnHome  = { PIN_BTN_HOME,  HIGH, HIGH, 0, false, false };
Button btnMode  = { PIN_BTN_MODE,  HIGH, HIGH, 0, false, false };
Button btnSpeak = { PIN_BTN_SPEAK, HIGH, HIGH, 0, false, false };

void updateButton(Button &b) {
  bool reading = digitalRead(b.pin);
  if (reading != b.lastReading) {
    b.lastReading = reading;
    b.lastChangeMs = millis();
  }
  if (millis() - b.lastChangeMs >= DEBOUNCE_MS) {
    if (reading != b.stable) {
      b.stable = reading;
      if (reading == LOW) b.pressed  = true;   // falling edge = press
      else                b.released = true;   // rising edge  = release
    }
  }
}
bool pressedEdge(Button &b)  { if (b.pressed)  { b.pressed  = false; return true; } return false; }
bool releasedEdge(Button &b) { if (b.released) { b.released = false; return true; } return false; }

// --------------------------------------------------------------- mode names --
const char* modeName(AppMode m) {
  switch (m) {
    case MODE_HOME:      return "home";
    case MODE_SUMMARIZE: return "summarize";
    case MODE_TODO:      return "todo";
  }
  return "home";
}
const char* modeDisplayName(AppMode m) {
  switch (m) {
    case MODE_HOME:      return "Home";
    case MODE_SUMMARIZE: return "Summarize";
    case MODE_TODO:      return "To-Do";
  }
  return "Home";
}

// -------------------------------------------------------------------- LED ----
void updateLed() {
  if (state == STATE_LISTENING) {
    digitalWrite(PIN_LED, HIGH);                             // steady on
    return;
  }
  if (state == STATE_THINKING) {
    digitalWrite(PIN_LED, (millis() / 250) % 2 ? HIGH : LOW); // blink
    return;
  }
  // IDLE / DISPLAY / BOOT: flash on ambient sound if the sensor is enabled.
  if (SOUND_SENSOR_ENABLED) {
    int level = analogRead(PIN_SOUND_A0);
    if (level > SOUND_THRESHOLD) lastSoundDetectMs = millis();
    bool flash = (millis() - lastSoundDetectMs) < SOUND_LED_ON_MS;
    digitalWrite(PIN_LED, flash ? HIGH : LOW);
    return;
  }
  digitalWrite(PIN_LED, LOW);
}

// ------------------------------------------------------------- text layout --
// Wrap responseText into displayLines[] at up to `maxChars` chars per line,
// honoring explicit '\n' newlines as forced line breaks.
void computeLines(int maxChars) {
  displayLineCount = 0;
  int len = responseText.length();
  int lineStart = 0;   // start of the current newline-delimited segment

  for (int i = 0; i <= len && displayLineCount < 32; i++) {
    bool atEnd   = (i == len);
    bool atBreak = (!atEnd && (responseText.charAt(i) == '\n' || responseText.charAt(i) == '\r'));
    if (!atEnd && !atBreak) continue;

    String seg = responseText.substring(lineStart, i);
    int segLen = seg.length();
    int s = 0;
    while (s < segLen && displayLineCount < 32) {
      int e = s + maxChars;
      if (e > segLen) e = segLen;
      // Prefer breaking at a space (unless a single word is longer than a line).
      if (e < segLen && seg.charAt(e) != ' ') {
        int space = seg.lastIndexOf(' ', e);
        if (space > s) e = space;
      }
      displayLines[displayLineCount++] = seg.substring(s, e);
      s = e;
      while (s < segLen && seg.charAt(s) == ' ') s++;
    }
    lineStart = i + 1;   // skip the newline character
  }
}

// Draw the wrapped lines with a vertical offset (used for scrolling).
void drawLines(int offsetY) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  int y = -offsetY;
  for (int i = 0; i < displayLineCount; i++) {
    display.setCursor(0, y);
    display.print(displayLines[i]);
    y += 8;
  }
  display.display();
}

// -------------------------------------------------------------- UI screens --
void drawIdle() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println("Whisper");
  display.setCursor(0, 12);
  display.print("Mode: "); display.println(modeDisplayName(mode));
  display.setCursor(0, 24);
  display.println("Hold btn3 to speak");
  display.setCursor(0, 36);
  display.println("btn1=Home btn2=Mode");
  display.setCursor(0, 54);
  display.print(WiFi.localIP().toString());
  display.display();
}

void drawListening() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(WHITE);
  display.setCursor(0, 8);
  display.println("Listening");
  display.setTextSize(1);
  display.setCursor(0, 40);
  display.println("release to send");
  display.display();
}

void drawThinking() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(WHITE);
  display.setCursor(0, 8);
  display.println("Thinking");
  display.setTextSize(1);
  display.setCursor(0, 40);
  display.println("...");
  display.display();
}

void showModeOverlay() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(WHITE);
  display.setCursor(0, 24);
  display.print(modeDisplayName(mode));
  display.setTextSize(1);
  display.display();
  redrawIdleAt = millis() + 1200;
}

#if AUDIO_SOURCE_ESP32
// Shown briefly after a hold-to-speak tap that was too short to record.
void showHoldToSpeakHint() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(WHITE);
  display.setCursor(0, 16);
  display.println("Hold to");
  display.println("speak");
  display.setTextSize(1);
  display.display();
  redrawIdleAt = millis() + 1200;
}
#endif

void showResponse(const String &text) {
  responseText = text;
  computeLines(21);
  int totalH = displayLineCount * 8;
  displayScrollMax = (totalH > 64) ? (totalH - 64) : 0;
  displayScrollOffset = 0;
  scrollPhase = SCROLL_TOP;
  scrollTimerMs = millis();
  displayStartedMs = millis();
  redrawIdleAt = 0;
  state = STATE_DISPLAY;
  drawLines(0);
}

void gotoIdle() {
  state = STATE_IDLE;
  redrawIdleAt = 0;
  drawIdle();
}

// ------------------------------------------------------------ state machine --
void callLaptop(const String &path);   // forward declaration (defined below)

void cycleMode() {
  if      (mode == MODE_HOME)      mode = MODE_SUMMARIZE;
  else if (mode == MODE_SUMMARIZE) mode = MODE_TODO;
  else                             mode = MODE_HOME;
}

void startListening() {
  if (state == STATE_LISTENING) return;
#if AUDIO_SOURCE_ESP32
  if (g_streaming) return;                     // previous utterance still finishing
  if (!micReady) {                             // mic init failed -> show error, don't engage
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0, 0);
    display.println("Mic init failed");
    display.println("Check INMP441 wiring");
    display.println("and 3.3V power.");
    display.display();
    return;
  }
  state = STATE_LISTENING;
  redrawIdleAt = 0;
  drawListening();
  pressStartMs    = millis();
  g_sessionId     = String(millis());
  g_chunkIndex    = 0;
  g_cancelSession = false;
  g_stopRequested = false;
  g_maxReached    = false;
  g_streaming     = true;
  xQueueReset(audioQueue);                     // drop any stale blocks
  callLaptop("/start_listening?session=" + g_sessionId + "&mode=" + String(modeName(mode)));
  xTaskNotifyGive(captureTaskHandle);
  xTaskNotifyGive(senderTaskHandle);
#else
  state = STATE_LISTENING;
  redrawIdleAt = 0;
  drawListening();
  callLaptop("/start_listening?mode=" + String(modeName(mode)));
#endif
}

void stopListening() {
  if (state != STATE_LISTENING) return;
#if AUDIO_SOURCE_ESP32
  if (!g_streaming) return;                    // never started (e.g. mic init failed)
  if (millis() - pressStartMs < MIN_RECORD_MS) {
    g_cancelSession = true;                    // tap too short -> discard, no transcribe
    g_stopRequested = true;
    state = STATE_IDLE;
    showHoldToSpeakHint();
    return;
  }
  g_stopRequested = true;                      // capture flushes; sender POSTs /stop_listening
  state = STATE_THINKING;
  thinkingStartedMs = millis();
  drawThinking();
#else
  state = STATE_THINKING;
  thinkingStartedMs = millis();
  drawThinking();
  callLaptop("/stop_listening");
#endif
}

void handleButtons() {
  updateButton(btnHome);
  updateButton(btnMode);
  updateButton(btnSpeak);

  if (pressedEdge(btnHome))  { mode = MODE_HOME; gotoIdle(); }
  if (pressedEdge(btnMode))  { cycleMode(); showModeOverlay(); state = STATE_IDLE; }
  if (pressedEdge(btnSpeak)) startListening();
  if (releasedEdge(btnSpeak)) stopListening();
}

void updateDisplayState() {
  // If we're waiting on the laptop and it never responds, recover to idle.
  if (state == STATE_THINKING) {
    if (millis() - thinkingStartedMs >= THINKING_TIMEOUT_MS) {
      showResponse("No response from laptop.\nCheck it is running.");
    }
    return;
  }
  if (state != STATE_DISPLAY) return;

  if (millis() - displayStartedMs >= DISPLAY_TIMEOUT_MS) { gotoIdle(); return; }
  if (displayScrollMax == 0) return;   // static text: nothing to animate

  switch (scrollPhase) {
    case SCROLL_TOP:
      if (millis() - scrollTimerMs >= SCROLL_PAUSE_MS) {
        scrollPhase = SCROLL_DOWN;
        scrollTimerMs = millis();
      }
      break;
    case SCROLL_DOWN:
      if (millis() - scrollTimerMs >= SCROLL_DELAY_MS) {
        scrollTimerMs = millis();
        if (displayScrollOffset < displayScrollMax) {
          displayScrollOffset++;
          drawLines(displayScrollOffset);
        } else {
          scrollPhase = SCROLL_BOTTOM;
          scrollTimerMs = millis();
        }
      }
      break;
    case SCROLL_BOTTOM:
      if (millis() - scrollTimerMs >= SCROLL_PAUSE_MS) {
        displayScrollOffset = 0;
        scrollPhase = SCROLL_TOP;
        scrollTimerMs = millis();
        drawLines(0);
      }
      break;
  }
}

// --------------------------------------------------------------- laptop HTTP --
void callLaptop(const String &path) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] WiFi not connected, skipping request");
    return;
  }
  HTTPClient http;
  http.setTimeout(3000);
  String url = String("http://") + LAPTOP_IP + ":" + String(LAPTOP_PORT) + path;
  http.begin(url);
  int code = http.GET();
  Serial.printf("[HTTP] GET %s -> %d\n", url.c_str(), code);
  http.end();
}

// ----------------------------------------------------------------- HTTP server --
void setupServer() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/plain", "Whisper Assistant ESP32 online");
  });
  server.on("/status", HTTP_GET, []() {
    server.send(200, "text/plain", "ok");
  });
  server.on("/mode", HTTP_GET, []() {
    server.send(200, "text/plain", modeName(mode));
  });
  server.on("/display", HTTP_POST, []() {
    String text = server.arg("plain");   // raw body (laptop sends text/plain)
    server.send(200, "text/plain", "ok");
    if (text.length() > 0) showResponse(text);
  });
  server.begin();
  Serial.println("[HTTP] ESP32 server started on port " + String(ESP_HTTP_PORT));
}

// --------------------------------------------------------------------- boot --
uint8_t scanOledAddress() {
  int found = -1;
  for (int addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[i2c] device found at 0x%02X\n", addr);
      if ((addr == 0x3C || addr == 0x3D) && found < 0) found = addr;
    }
  }
  return (found >= 0) ? (uint8_t)found : (uint8_t)OLED_ADDR;
}

void connectWiFi() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println("Whisper");
  display.println("");
  display.print("WiFi: "); display.println(WIFI_SSID);
  display.print("Connecting...");
  display.display();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[wifi] connecting to %s", WIFI_SSID);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] FAILED. Check WIFI_SSID/WIFI_PASSWORD in config.h.");
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("WiFi failed");
    display.println("Check SSID/password");
    display.println("in config.h");
    display.display();
    while (1) {   // blink LED to signal a hard fault
      digitalWrite(PIN_LED, !digitalRead(PIN_LED));
      delay(300);
    }
  }
  Serial.println("[wifi] connected, IP = " + WiFi.localIP().toString());
}

void showReadyIp() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println("Ready!");
  display.println("");
  display.println("IP:");
  display.println(WiFi.localIP().toString());
  display.display();
}

// --------------------------------------------------------------------- setup --
void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(PIN_BTN_HOME,  INPUT_PULLUP);
  pinMode(PIN_BTN_MODE,  INPUT_PULLUP);
  pinMode(PIN_BTN_SPEAK, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  // GPIO34/35 are input-only (no internal pull-ups); the sound module drives them.
  pinMode(PIN_SOUND_A0, INPUT);
  pinMode(PIN_SOUND_D0, INPUT);

  Wire.begin(OLED_SDA, OLED_SCL);
  uint8_t oledAddr = scanOledAddress();
  if (!display.begin(SSD1306_SWITCHCAPVCC, oledAddr)) {
    Serial.println("[oled] SSD1306 init failed");
    while (1) { digitalWrite(PIN_LED, HIGH); delay(150); digitalWrite(PIN_LED, LOW); delay(150); }
  }
  Wire.setClock(400000);
  display.clearDisplay();
  display.setTextColor(WHITE);
  Serial.printf("[oled] SSD1306 ready at 0x%02X\n", oledAddr);

  connectWiFi();
  setupServer();
#if AUDIO_SOURCE_ESP32
  micReady = mic_i2s_init();
  if (micReady) {
    audioQueue = xQueueCreate(AUDIO_QUEUE_LEN, sizeof(AudioBlock));
    BaseType_t c1 = xTaskCreatePinnedToCore(captureTask, "cap", 8192, NULL, 5, &captureTaskHandle, 1);
    BaseType_t c2 = xTaskCreatePinnedToCore(senderTask,  "snd", 8192, NULL, 3, &senderTaskHandle,  0);
    if (audioQueue == NULL || c1 != pdPASS || c2 != pdPASS) {
      Serial.println("[audio] queue/task setup failed — audio disabled");
      micReady = false;
    } else {
      Serial.println("[audio] capture + sender tasks started");
    }
  } else {
    Serial.println("[i2s] INMP441 init FAILED — audio disabled");
  }
#endif
  showReadyIp();
  bootIpShownAt = millis();
  state = STATE_BOOT;
}

// ---------------------------------------------------------------------- loop --
void loop() {
  server.handleClient();   // always service inbound HTTP

  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiCheck > 10000) {
    lastWifiCheck = millis();
    WiFi.reconnect();
  }

  handleButtons();
#if AUDIO_SOURCE_ESP32
  if (g_maxReached) {                          // capture hit MAX_RECORD_MS: move UI to "Thinking"
    g_maxReached = false;
    if (state == STATE_LISTENING) {
      state = STATE_THINKING;
      thinkingStartedMs = millis();
      drawThinking();
    }
  }
#endif
  updateLed();
  updateDisplayState();

  if (state == STATE_BOOT && millis() - bootIpShownAt >= BOOT_IP_SHOW_MS) {
    gotoIdle();
  }
  if (redrawIdleAt && millis() >= redrawIdleAt) {
    redrawIdleAt = 0;
    if (state == STATE_IDLE) drawIdle();
  }

  delay(1);
}
