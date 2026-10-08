# Whisper Assistant — ESP32-mic version

This folder is the **on-device microphone** version of the Whisper Assistant.
Instead of the laptop's microphone, an **INMP441 I2S MEMS mic on the ESP32**
captures your voice while you hold button 3, and streams it to the laptop over
Wi-Fi. The laptop still does all the thinking (speech-to-text → DeepSeek →
Markdown logging) — the ESP32 just adds audio capture.

The original laptop-mic version lives at the repo root (`esp32_firmware/` +
`laptop_app/`). Everything here is a self-contained copy of that, plus the mic.

```
                 local Wi-Fi
   ┌─────────┐  HTTP (port 5000)   ┌──────────────┐
   │  LAPTOP │ <------------------ │     ESP32     │
   │  "brain"│  /audio_chunk (PCM) │  mic + OLED   │
   │  STT+LLM│ ------------------> │  + buttons    │
   └─────────┘  POST /display      └──────────────┘
```

---

## 1. Parts you need

| Part | Qty | Notes |
|------|-----|-------|
| ESP32 dev board | 1 | 30 or 38-pin, ESP32-WROOM |
| INMP441 I2S MEMS mic breakout | 1 | use one (a second is a spare) |
| 0.96" OLED SSD1306 (128×64) | 1 | I2C display |
| Tactile push buttons | 3 | Home / Mode / Hold-to-speak |
| LED + 220–330 Ω resistor | 1 | status light |
| Jumper wires + breadboard | — | or a perfboard if soldering |
| USB **data** cable | 1 | not a charge-only cable |

---

## 2. Pin labels — "D" vs bare numbers

Different ESP32 boards label pins differently, but they mean the same thing:

- On boards with **bare numbers** (`14`, `27`, `13`), the number is the GPIO.
- On boards with a **"D" prefix** (`D14`, `D27`, `D13`), **the number after the
  "D" is still the GPIO** — `D14` = GPIO 14.

So "GPIO 14" means the pin labeled `14` **or** `D14`, whichever your board shows.
(Note: this is *different* from ESP8266 NodeMCU/D1-mini boards, where `D#` ≠
GPIO #. This project needs an **ESP32** — check the metal shield says "ESP32".)

---

## 3. Wiring (everything)

Use the **3V3** pin for 3.3 V — **never** 5 V for the OLED or mic.

### OLED (display)
| OLED pin | → | ESP32 |
|---|---|---|
| VCC | → | **3V3** |
| GND | → | **GND** |
| SDA | → | **21** (`D21`) |
| SCL | → | **22** (`D22`) |

### Buttons (each button has 2 legs)
| Button | leg 1 → | leg 2 → |
|---|---|---|
| HOME | **32** (`D32`) | GND |
| MODE | **33** (`D33`) | GND |
| HOLD-TO-SPEAK | **25** (`D25`) | GND |

### LED
| LED leg | → |
|---|---|
| Long leg (anode) | resistor → **26** (`D26`) |
| Short leg (cathode) | **GND** |

### INMP441 mic (3.3 V ONLY)
| INMP441 pin | → | ESP32 |
|---|---|---|
| VDD | → | **3V3** |
| GND | → | **GND** |
| SCK | → | **14** (`D14`) |
| WS | → | **27** (`D27`) |
| SD | → | **13** (`D13`) |
| L/R | → | **GND** (selects the LEFT channel — do not skip) |

> If your mic board has a **CHIPEN** pin, tie it to 3V3 too. Most INMP441
> breakouts have exactly the 6 pins above.

---

## 4. Soldering / assembly

**Easiest (recommended): breadboard, no soldering.**
1. Solder **male header pins** onto the ESP32 (and OLED/mic, if they have holes).
2. Push everything into a breadboard and connect with jumper wires per §3.
3. Test on the breadboard first, then move to a permanent board.

**Soldering to a perfboard:**
1. Solder header pins to the ESP32 first.
2. Mount ESP32, OLED, mic, buttons, LED.
3. Wire point-to-point per §3. **Double-check 3V3 vs 5V before soldering power.**
4. Tips: heat the joint (wire + pad) ~1–2 s, then feed solder to the joint (not
   the iron); a shiny joint is good, a dull ball is a cold joint (reflow it);
   watch for bridges between 13/14/27 (they sit close together).

---

## 5. Firmware setup

Edit `firmware/src/config.h` — set your Wi-Fi and laptop IP. The mic flag is
**already `1`** in this folder (that's the point of this version):

```c
#define WIFI_SSID      "YOUR_WIFI_NAME"
#define WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"
#define LAPTOP_IP      "192.168.1.100"   // your laptop's local IP
#define AUDIO_SOURCE_ESP32 1             // already 1 = INMP441 mic (0 = laptop mic)
```

Find your laptop's IP: **Windows** `ipconfig` · **Mac** `ipconfig getifaddr en0`
· **Linux** `ip a`.

---

## 6. Flash the mic test sketch FIRST

1. In VS Code, open the folder **`firmware/tests/i2s_mic_test`**.
2. Plug in the ESP32, click **Upload**.
3. Open the **Serial Plotter** at **115200 baud**.
4. Talk or tap the mic — the trace should **jump**. Flat `0` = wiring/channel
   problem (check L/R→GND, SD→13).

Then open **`firmware`** and click **Upload** to flash the main firmware.

---

## 7. Laptop setup

1. Copy the template: `cp laptop/.env.example laptop/.env` (Windows: `copy`).
2. Edit `laptop/.env`:
   ```
   DEEPSEEK_API_KEY=sk-xxxxxxxxxxxxxxxx
   AUDIO_SOURCE=esp32
   DEBUG_AUDIO=true       # optional — saves laptop/debug_audio/last.wav
   ```
3. Install + run:
   ```bash
   pip install -r laptop/requirements.txt
   python laptop/main.py
   ```

> **The two sides must agree:** `AUDIO_SOURCE_ESP32 1` in `config.h` ⇔
> `AUDIO_SOURCE=esp32` in `.env`.

---

## 8. Demo checklist

- [ ] Mic wired per §3; test sketch (§6) shows a live level.
- [ ] `AUDIO_SOURCE_ESP32 1` in `config.h`; main firmware flashed.
- [ ] `AUDIO_SOURCE=esp32` in `.env`; laptop app running.
- [ ] Both devices on the **same Wi-Fi**.
- [ ] Hold button 3 → "Listening" + LED on → speak → release → "Thinking" → answer.
- [ ] A tap under ~0.3 s shows "Hold to speak" and does nothing.
- [ ] Holding >20 s auto-stops and still answers.
- [ ] (If `DEBUG_AUDIO=true`) `python laptop/tools/play_last_audio.py` plays your voice.

---

## 9. Troubleshooting

**Silent audio / flat line on the plotter**
→ L/R is not on GND (LEFT channel), or SD/SCK/WS wired wrong, or VDD is on 5 V.

**Noise / garbage / robotic audio**
→ Wrong slot width or shift. The INMP441 is 24-bit in a 32-bit slot; the default
`I2S_PCM_SHIFT 16` is unity. Re-flash the test sketch and confirm the trace moves
with your voice (not a random full-scale hash). Check wiring/clock before changing
the shift.

**Audio too quiet**
→ Lower `I2S_PCM_SHIFT` to 15 or 14 (+6 / +12 dB) or raise `I2S_GAIN_PERCENT`.

**Clipping / distortion**
→ Raise `I2S_PCM_SHIFT` toward 16 (or above) or lower `I2S_GAIN_PERCENT`.

**Choppy / stuttering**
→ Try a smaller `I2S_CHUNK_SAMPLES` (512) or larger `AUDIO_QUEUE_LEN` (32) in
`config.h`. Keep both devices on the same network (no guest isolation).

**Wrong speed / pitch**
→ Sample-rate mismatch. `I2S_SAMPLE_RATE` in `config.h` must be 16000 (it is).

**"WiFi failed" on OLED** → wrong SSID/password in `config.h`.

**"Thinking…" forever** → laptop app not running, `LAPTOP_IP` wrong, or not the
same network.

**"Mic init failed" on OLED** → I2S driver failed; check wiring + serial `[i2s]`
log, and that no other pin reuses 13/14/27.

---

## 10. Layout

```
esp32-mic/
├── README.md               # this file
├── firmware/               # ESP32 firmware (INMP441 mic capture + streaming)
│   ├── platformio.ini
│   ├── src/
│   │   ├── config.h        # Wi-Fi + IP + pins + audio tunables (EDIT THIS)
│   │   ├── mic_i2s.h       # I2S init + 24-bit->16-bit conversion
│   │   └── main.cpp        # OLED/buttons/LED + capture/sender tasks
│   └── tests/i2s_mic_test/ # mic bring-up sketch (flash this first)
└── laptop/                 # laptop app (STT + DeepSeek + logging)
    ├── main.py
    ├── requirements.txt
    ├── .env.example
    └── tools/play_last_audio.py
```
