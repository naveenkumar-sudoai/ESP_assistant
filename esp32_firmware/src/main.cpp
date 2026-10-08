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
  state = STATE_LISTENING;
  redrawIdleAt = 0;
  drawListening();
  callLaptop("/start_listening?mode=" + String(modeName(mode)));
}

void stopListening() {
  if (state != STATE_LISTENING) return;
  state = STATE_THINKING;
  thinkingStartedMs = millis();
  drawThinking();
  callLaptop("/stop_listening");
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
