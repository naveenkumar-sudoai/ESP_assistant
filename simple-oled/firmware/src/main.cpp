// =============================================================================
//  Whisper Assistant (Simple) — ESP32 OLED-only "face".
//
//  Responsibilities:
//    - Connect to Wi-Fi and run a tiny HTTP server.
//    - Register with the laptop on boot (so the laptop learns this ESP32's IP).
//    - Drive the SSD1306 OLED: show status, then show whatever text the laptop
//      POSTs to /display (vertical auto-scroll if it's long).
//
//  No buttons, no LED, no mic. All speech happens on the laptop; this ESP32 is
//  purely the wireless display.
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>

#include "config.h"

Adafruit_SSD1306 display(128, 64, &Wire, -1);   // -1 = no hardware reset pin
WebServer server(ESP_HTTP_PORT);

String  responseText      = "";
String  displayLines[32];
int     displayLineCount  = 0;
int     displayScrollMax  = 0;    // 0 => fits on screen (static)
int     displayScrollOffset = 0;
enum    ScrollPhase { SCROLL_TOP, SCROLL_DOWN, SCROLL_BOTTOM };
ScrollPhase scrollPhase = SCROLL_TOP;

unsigned long displayStartedMs = 0;
unsigned long scrollTimerMs     = 0;
unsigned long bootIpShownAt     = 0;
unsigned long lastRegisterMs    = 0;

// ------------------------------------------------------------- text layout --
void computeLines(int maxChars) {
  displayLineCount = 0;
  int len = responseText.length();
  int lineStart = 0;
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
      if (e < segLen && seg.charAt(e) != ' ') {
        int space = seg.lastIndexOf(' ', e);
        if (space > s) e = space;
      }
      displayLines[displayLineCount++] = seg.substring(s, e);
      s = e;
      while (s < segLen && seg.charAt(s) == ' ') s++;
    }
    lineStart = i + 1;
  }
}

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
  display.setCursor(0, 16);
  display.println("Speak on the laptop");
  display.setCursor(0, 28);
  display.println("(press ENTER there)");
  display.setCursor(0, 54);
  display.print(WiFi.localIP().toString());
  display.display();
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
  drawLines(0);
}

void updateScroll() {
  if (displayStartedMs == 0) return;
  if (millis() - displayStartedMs >= DISPLAY_TIMEOUT_MS) {   // back to idle
    displayStartedMs = 0;
    drawIdle();
    return;
  }
  if (displayScrollMax == 0) return;                         // static text

  switch (scrollPhase) {
    case SCROLL_TOP:
      if (millis() - scrollTimerMs >= SCROLL_PAUSE_MS) { scrollPhase = SCROLL_DOWN; scrollTimerMs = millis(); }
      break;
    case SCROLL_DOWN:
      if (millis() - scrollTimerMs >= SCROLL_DELAY_MS) {
        scrollTimerMs = millis();
        if (displayScrollOffset < displayScrollMax) { displayScrollOffset++; drawLines(displayScrollOffset); }
        else { scrollPhase = SCROLL_BOTTOM; scrollTimerMs = millis(); }
      }
      break;
    case SCROLL_BOTTOM:
      if (millis() - scrollTimerMs >= SCROLL_PAUSE_MS) { displayScrollOffset = 0; scrollPhase = SCROLL_TOP; scrollTimerMs = millis(); drawLines(0); }
      break;
  }
}

// ------------------------------------------------------ register with laptop --
void registerWithLaptop() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.setTimeout(3000);
  String url = String("http://") + LAPTOP_IP + ":" + String(LAPTOP_PORT) + "/register";
  http.begin(url);
  int code = http.POST("");   // empty body; the laptop just needs the source IP
  Serial.printf("[http] register -> %d\n", code);
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
  server.on("/display", HTTP_POST, []() {
    String text = server.arg("plain");   // raw body (laptop sends text/plain)
    server.send(200, "text/plain", "ok");
    if (text.length() > 0) showResponse(text);
  });
  server.begin();
  Serial.println("[http] ESP32 server started on port " + String(ESP_HTTP_PORT));
}

// --------------------------------------------------------------------- boot --
uint8_t scanOledAddress() {
  int found = -1;
  for (int addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
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
    while (1) { delay(1000); }
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

  Wire.begin(OLED_SDA, OLED_SCL);
  uint8_t oledAddr = scanOledAddress();
  if (!display.begin(SSD1306_SWITCHCAPVCC, oledAddr)) {
    Serial.println("[oled] SSD1306 init failed");
    while (1) { delay(500); }
  }
  Wire.setClock(400000);
  display.clearDisplay();
  display.setTextColor(WHITE);
  Serial.printf("[oled] SSD1306 ready at 0x%02X\n", oledAddr);

  connectWiFi();
  setupServer();
  registerWithLaptop();
  showReadyIp();
  bootIpShownAt = millis();
}

// ---------------------------------------------------------------------- loop --
void loop() {
  server.handleClient();

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
  } else if (millis() - lastRegisterMs >= REGISTER_INTERVAL_MS) {
    lastRegisterMs = millis();
    registerWithLaptop();
  }

  if (bootIpShownAt && millis() - bootIpShownAt >= BOOT_IP_SHOW_MS) {
    bootIpShownAt = 0;
    drawIdle();
  }

  updateScroll();
  delay(1);
}
