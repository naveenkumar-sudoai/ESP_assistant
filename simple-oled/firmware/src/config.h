#ifndef CONFIG_H
#define CONFIG_H

// =============================================================================
//  Whisper Assistant (Simple) — ESP32 CONFIGURATION
//
//  EDIT THIS FILE BEFORE FLASHING. This version is OLED-only: no buttons, no
//  LED, no mic. The ESP32 is just a wireless display for the laptop.
// =============================================================================

// ---------------------------------------------------------------------------
// Wi-Fi (the ESP32 and the laptop MUST be on the same network).
// ---------------------------------------------------------------------------
#define WIFI_SSID      "YOUR_WIFI_NAME"
#define WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"

// ---------------------------------------------------------------------------
// Laptop "brain" address. The ESP32 registers itself here on boot (and every
// minute), so the laptop learns this ESP32's IP and can push text back to it.
// ---------------------------------------------------------------------------
#define LAPTOP_IP    "192.168.1.100"
#define LAPTOP_PORT  5000

// ---------------------------------------------------------------------------
// ESP32's own HTTP server port (the laptop POSTs /display here).
// ---------------------------------------------------------------------------
#define ESP_HTTP_PORT 80

// ---------------------------------------------------------------------------
// OLED (I2C). Standard pins; only change if you rewired.
// ---------------------------------------------------------------------------
#define OLED_SDA   21
#define OLED_SCL   22
#define OLED_ADDR  0x3C   // auto-detects 0x3C/0x3D anyway

// ---------------------------------------------------------------------------
// Timing.
// ---------------------------------------------------------------------------
#define DISPLAY_TIMEOUT_MS   15000   // how long an answer stays on screen (ms)
#define SCROLL_DELAY_MS      25      // ms per 1px vertical scroll step
#define SCROLL_PAUSE_MS      1200    // pause at top/bottom of a scroll (ms)
#define BOOT_IP_SHOW_MS      4000    // how long the IP is shown at boot (ms)
#define REGISTER_INTERVAL_MS 60000   // re-register with the laptop every 60 s

#endif
