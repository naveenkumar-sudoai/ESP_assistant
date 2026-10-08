# Whisper Assistant — Simple (OLED-only)

The **simplest** version. Just an **ESP32 + OLED** — no buttons, no LED, no mic
on the ESP32. You speak into the **laptop's microphone**, and **DeepSeek decides
on its own** what you want (a question, something to summarize, or a to-do list),
then answers. The answer is pushed to the ESP32 and shown on the OLED.

```
   ┌─────────┐  Wi-Fi (HTTP)     ┌──────────────┐
   │  LAPTOP │  POST /display    │     ESP32     │
   │ mic+STT │ ----------------> │   OLED only   │
   │ +DeepSeek│                  │  (no buttons) │
   └─────────┘                   └──────────────┘
```

---

## 1. Parts you need

| Part | Notes |
|------|-------|
| ESP32 dev board | the "D"-labeled board |
| 0.96" OLED SSD1306 (128×64) | the display |
| USB data cable + jumper wires | |

That's it — **no buttons, no LED, no mic breakout**.

---

## 2. Wiring (OLED only)

| OLED pin | → | ESP32 |
|---|---|---|
| VCC | → | **3V3** (not 5 V) |
| GND | → | **GND** |
| SDA | → | **21** (`D21`) |
| SCL | → | **22** (`D22`) |

> On boards with a "D" prefix, `D21` = GPIO 21 and `D22` = GPIO 22 (the number
> after "D" is the GPIO).

---

## 3. Firmware setup

Edit `firmware/src/config.h` — set your Wi-Fi and the laptop's IP:

```c
#define WIFI_SSID      "YOUR_WIFI_NAME"
#define WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"
#define LAPTOP_IP      "192.168.1.100"   // your laptop's local IP
```

Then open the **`firmware`** folder in PlatformIO and click **Upload**.

> Find your laptop's IP: **Windows** `ipconfig` · **Mac** `ipconfig getifaddr en0`
> · **Linux** `ip a`.

---

## 4. Laptop setup

1. Copy the template: `cp laptop/.env.example laptop/.env` (Windows: `copy`).
2. Edit `laptop/.env`:
   ```
   DEEPSEEK_API_KEY=sk-xxxxxxxxxxxxxxxx
   ```
3. Install + run:
   ```bash
   pip install -r laptop/requirements.txt
   python laptop/main.py
   ```

> `ESP32_IP` is left empty — the app learns it automatically when the ESP32
> registers on boot.

---

## 5. How to use

1. Power on the ESP32 → OLED shows `Ready!` + its IP.
2. On the laptop, the app says **"Press ENTER to start talking"**.
3. Press **ENTER**, speak into the laptop mic, then press **ENTER** again.
4. DeepSeek figures out what you want and answers — the short answer appears on
   the OLED (and in the laptop terminal).

It auto-classifies:
- **a question** → answers it,
- **something to summarize** → one-sentence summary,
- **tasks / things to do** → a short to-do list.

No buttons to switch mode — DeepSeek sorts it out.

---

## 6. Troubleshooting

**OLED blank** → check SDA→21, SCL→22, VCC on 3V3 (not 5 V).

**"WiFi failed" on OLED** → wrong SSID/password in `config.h`.

**Laptop says "ESP32 IP unknown"** → the ESP32 didn't register. Check:
1. The laptop app is running.
2. `LAPTOP_IP` in `config.h` is the laptop's local IP.
3. Both devices are on the same Wi-Fi (no guest isolation), and your firewall
   allows Python through port 5000.

**"didn't catch that"** → check the laptop mic works (record in any OS app). On
Linux, install PortAudio (`sudo apt-get install libportaudio2` / `dnf install portaudio`).

**First answer is slow (30–60 s)** → normal: faster-whisper downloads its model
once. Set `STT_BACKEND=openai` (+ an `OPENAI_API_KEY`) to skip that.

**Answer looks cut off / weird** → the OLED strips non-ASCII and caps at ~90
chars; the full text is printed in the laptop terminal.

---

## 7. Layout

```
simple-oled/
├── README.md            # this file
├── firmware/            # ESP32 OLED-only firmware
│   ├── platformio.ini
│   └── src/
│       ├── config.h     # Wi-Fi + laptop IP + OLED pins (EDIT THIS)
│       └── main.cpp     # OLED + Wi-Fi + /display + register
└── laptop/              # laptop app (mic + STT + DeepSeek auto-classify)
    ├── main.py
    ├── requirements.txt
    └── .env.example
```
