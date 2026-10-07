#ifndef CONFIG_H
#define CONFIG_H

// =============================================================================
//  Whisper Assistant — ESP32 CONFIGURATION
//
//  EDIT THIS FILE BEFORE FLASHING. Every user-tunable value lives here, kept
//  separate from the logic in main.cpp so you never have to touch the code.
// =============================================================================

// ---------------------------------------------------------------------------
// Wi-Fi (the ESP32 and the laptop MUST be on the same network).
// ---------------------------------------------------------------------------
#define WIFI_SSID      "YOUR_WIFI_NAME"
#define WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"

// ---------------------------------------------------------------------------
// Laptop "brain" server address.
// Use the laptop's LOCAL IP (e.g. 192.168.1.x). Find it with:
//   Windows:   ipconfig   -> look for "IPv4 Address"
//   Mac/Linux: ip a | grep inet   (or: ifconfig)
// The port must match LAPTOP_PORT in the laptop's .env (default 5000).
// ---------------------------------------------------------------------------
#define LAPTOP_IP    "192.168.1.100"
#define LAPTOP_PORT  5000

// ---------------------------------------------------------------------------
// ESP32's own HTTP server port. The laptop POSTs the response back here.
// Keep 80 unless you also change ESP32_PORT in the laptop's .env.
// ---------------------------------------------------------------------------
#define ESP_HTTP_PORT 80

// ---------------------------------------------------------------------------
// Pin mapping (ESP32 DevKitC / WROOM-32). See the README "Wiring" table.
// All of these are safe, non-boot, non-strapping GPIOs — validated.
// ---------------------------------------------------------------------------
#define PIN_BTN_HOME   32   // Button 1 — HOME / reset mode
#define PIN_BTN_MODE   33   // Button 2 — cycle mode (Home -> Summarize -> To-Do)
#define PIN_BTN_SPEAK  25   // Button 3 — HOLD TO SPEAK
#define PIN_LED        26   // LED (listening / thinking / sound indicator)
#define PIN_SOUND_A0   34   // Loudness sensor analog out (input-only pin)
#define PIN_SOUND_D0   35   // Loudness sensor digital out (optional; unused by default)

// I2C for the SSD1306 OLED (standard pins; only change if you rewired).
#define OLED_SDA   21
#define OLED_SCL   22
#define OLED_ADDR  0x3C   // 0x3C is most common; the firmware auto-detects 0x3C/0x3D anyway

// ---------------------------------------------------------------------------
// Tunable behaviour.
// ---------------------------------------------------------------------------
#define DEBOUNCE_MS         30      // button debounce window (ms)
#define DISPLAY_TIMEOUT_MS  15000   // how long a response stays on screen (ms)
#define THINKING_TIMEOUT_MS 90000   // give up waiting for the laptop (ms). Kept long
                                    // because the first faster-whisper run downloads
                                    // its model (30-60s) before transcribing.
#define SCROLL_DELAY_MS     25      // ms per 1px vertical scroll step (lower = faster)
#define SCROLL_PAUSE_MS     1200    // pause at top/bottom of a scroll (ms)
#define BOOT_IP_SHOW_MS     4000    // how long the IP is shown at boot (ms)

// Loudness sensor (OPTIONAL — leave disabled if the module is not wired).
// Purely cosmetic: lights the LED on ambient sound. Not part of speech capture.
#define SOUND_SENSOR_ENABLED false
#define SOUND_THRESHOLD      2000   // analog level (0..4095) above which the LED lights
#define SOUND_LED_ON_MS      120    // how long the LED stays lit after detection

#endif
