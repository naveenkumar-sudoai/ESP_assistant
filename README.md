# Whisper Assistant

A two-part, hardware voice assistant for a demo deadline:

- **Part A — the "brain"** (`laptop_app/`): a Python app on your laptop that
  records your voice, turns it into text (speech-to-text), sends it to the
  DeepSeek API for reasoning, and pushes a short answer back to the device.
- **Part B — the "face"** (`esp32_firmware/`): an ESP32 with a small OLED
  screen, three buttons, an LED, and (optionally) a sound sensor. It shows
  status + answers on the screen and tells the laptop when you're speaking.

They talk to each other over your **local Wi-Fi network** with plain HTTP.
No cloud server, no app store, no accounts beyond a DeepSeek API key.

---

## 1. What you need

### Hardware
| Part | Notes |
|------|-------|
| ESP32 Dev Board (30/38-pin, ESP32-WROOM) | The main board |
| 0.96" OLED, SSD1306, I2C (128×64) | The display |
| 3 × tactile push buttons | Normally-open, momentary |
| 1 × LED + ~220–330 Ω resistor | Status indicator |
| KY-038 sound sensor (optional) | Cosmetic "listening" LED trigger |
| USB cable + breadboard/jumper wires | |

### Software (on the laptop)
- **Python 3.9–3.12** (3.13 works too; get it from https://python.org)
- **git** (https://git-scm.com)
- **PlatformIO** for flashing the ESP32 (see §4)

> **ESP8266 (NodeMCU) fallback note:** this project is built for the ESP32.
> An ESP8266 *can* be adapted (it has the same libraries) but the pin map,
> ADC range, and I2C pins differ. If you only have an ESP8266, the pin
> numbers in `config.h` must be changed (e.g. use `D1`/`D2` for I2C, and note
> the 0–1023 ADC range). This guide assumes an ESP32.

---

## 2. System overview

```
                 local Wi-Fi
   ┌─────────┐  HTTP (port 5000)   ┌──────────────┐
   │  LAPTOP │ <------------------ │     ESP32     │
   │  "brain"│  GET /start_listening│   "face"      │
   │         │  GET /stop_listening │               │
   │  STT+LLM│ ------------------> │  OLED+buttons │
   └─────────┘  POST /display      └──────────────┘
                   (port 80)
```

1. You **hold** button 3 (hold-to-speak).
2. The ESP32 tells the laptop to start recording, and shows "Listening".
3. You **release** the button. The ESP32 shows "Thinking".
4. The laptop runs speech-to-text → DeepSeek → a short answer, then posts it
   to the ESP32, which shows it on the OLED (scrolling if it's long).

The laptop **learns the ESP32's IP automatically** the first time the ESP32
contacts it — you only need to tell the *ESP32* the *laptop's* IP (once,
before flashing).

---

## 3. Set up the laptop app (Part A)

Do this once. Commands for **Windows** and **Mac/Linux** are shown separately
where they differ — you don't know which laptop you'll be on.

### 3.1 Clone the repo

```bash
git clone https://github.com/naveenkumar-sudoai/ESP_assistant.git
cd ESP_assistant
cd laptop_app
```

### 3.2 Create + activate a virtual environment

**Windows — Command Prompt:**
```bat
py -3 -m venv venv
venv\Scripts\activate.bat
```

**Windows — PowerShell:**
```powershell
py -3 -m venv venv
.\venv\Scripts\Activate.ps1
```
> If PowerShell blocks the script, run `Set-ExecutionPolicy -Scope Process Bypass`
> first, then re-run the activation line. If `py` isn't found, use
> `python -m venv venv` instead.

**Mac / Linux:**
```bash
python3 -m venv venv
source venv/bin/activate
```

You should now see `(venv)` at the start of your prompt.

### 3.3 Install dependencies

```bash
python -m pip install --upgrade pip
pip install -r requirements.txt
```

This installs Flask, `sounddevice` (microphone), `faster-whisper` (local
speech-to-text), `numpy`, `requests`, `python-dotenv`, and `openai` (optional
fallback). It's a few hundred MB total — let it finish.

> **Linux only:** `sounddevice` needs the system **PortAudio** library (it's
> bundled on Windows and macOS, but not Linux). If you later see
> `PortAudio library not found`, install it once:
> Debian/Ubuntu → `sudo apt-get install libportaudio2` ·
> Fedora → `sudo dnf install portaudio`.

> **If `faster-whisper` fails to install** (rare), you can skip it by using the
> OpenAI speech-to-text backend instead — see §3.5, point 2.

### 3.4 Create the `.env` file

Copy the template and edit it:

**Windows:**
```bat
copy .env.example .env
notepad .env
```

**Mac / Linux:**
```bash
cp .env.example .env
nano .env
```

Put your DeepSeek API key in it:

```
DEEPSEEK_API_KEY=sk-xxxxxxxxxxxxxxxx
```

Everything else has a sensible default. See the table:

| Key | Meaning | Default |
|-----|---------|---------|
| `DEEPSEEK_API_KEY` | **Required.** Your DeepSeek key | *(none)* |
| `DEEPSEEK_MODEL` | DeepSeek model | `deepseek-chat` |
| `STT_BACKEND` | `faster-whisper` (local) or `openai` (API) | `faster-whisper` |
| `WHISPER_MODEL` | faster-whisper model size (`tiny`/`base`/`small`) | `base` |
| `OPENAI_API_KEY` | Only if `STT_BACKEND=openai` | *(empty)* |
| `ESP32_IP` | Leave empty (auto-detected) | *(empty)* |
| `ESP32_PORT` | The ESP32's HTTP port | `80` |
| `LAPTOP_HOST` | Keep `0.0.0.0` so the ESP32 can reach you | `0.0.0.0` |
| `LAPTOP_PORT` | Port the laptop listens on | `5000` |

> **Where do I get a DeepSeek API key?** Go to https://platform.deepseek.com,
> sign up, and create an API key in the dashboard. Paste it after
> `DEEPSEEK_API_KEY=`.

### 3.5 Speech-to-text choice (important)

The default, `faster-whisper`, runs **locally** — no extra API key — but it
downloads a ~145 MB model from Hugging Face the **first time** you speak. Make
sure you have internet, and expect the very first transcription to take an
extra 30–60 s while the model downloads (after that it's cached and instant).

If you'd rather not download a local model:

1. Set `STT_BACKEND=openai` in `.env`.
2. Add your OpenAI key: `OPENAI_API_KEY=sk-...`.
3. That's it — no local model download; each phrase is sent to OpenAI's
   Whisper API instead.

---

## 4. Flash the ESP32 firmware (Part B)

### 4.1 Install PlatformIO

The easiest way is the **PlatformIO VS Code extension**:
1. Install VS Code from https://code.visualstudio.com.
2. Open the Extensions panel (Ctrl+Shift+X / Cmd+Shift+X) and install
   **"PlatformIO IDE"**.
3. Restart VS Code.

(You can also use the standalone PlatformIO CLI — `pip install platformio` —
but the VS Code extension is the most reliable path.)

### 4.2 Edit Wi-Fi credentials **before** flashing

Open `esp32_firmware/src/config.h` and edit **only** the values marked below:

```c
#define WIFI_SSID      "YOUR_WIFI_NAME"      // <-- your network name
#define WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"  // <-- your network password

#define LAPTOP_IP    "192.168.1.100"   // <-- your laptop's local IP (see §4.3)
#define LAPTOP_PORT  5000              // matches LAPTOP_PORT in .env
```

Save the file.

### 4.3 Find your laptop's local IP

The ESP32 must know where to send data. Find the laptop's **local** IP (the
one on your home network, usually `192.168.x.x` or `10.0.x.x`):

- **Windows:** open Command Prompt and run `ipconfig` → look for
  **"IPv4 Address"** under your Wi-Fi/Ethernet adapter.
- **Mac:** System Settings → Network → Wi-Fi → your IP, or run `ipconfig getifaddr en0`.
- **Linux:** run `ip a` or `ifconfig` → look for the `inet` address.

Paste that into `LAPTOP_IP` in `config.h`. **Do not use `127.0.0.1`** — that
only works on the laptop itself.

> Tip: when you run the laptop app, it also prints your likely IP at startup.

### 4.4 Build & flash

> This project is **pre-configured for the `esp32dev` board** (generic ESP32
> DevKitC / WROOM-32) in `platformio.ini` — you do **not** pick a board by hand.

1. In VS Code, open the **`esp32_firmware`** folder
   (`File → Open Folder → …/esp32_firmware`).
2. Connect the ESP32 with a USB cable.
3. Check the board is detected: click the PlatformIO **"Devices"** / serial
   port selector in the bottom toolbar and pick your port:
   - **Windows:** `COM3` (or similar)
   - **Linux:** `/dev/ttyUSB0` (or `/dev/ttyACM0`)
   - **macOS:** `/dev/cu.SLAB_USBtoUART` (CP210x) or `/dev/cu.usbserial-*` (CH340)
4. **macOS only:** if no serial port appears, install the USB-serial driver
   first — the **CP210x** driver from Silicon Labs, or the **CH340** driver
   (whichever chip your board uses; most DevKitC boards use CP210x).
5. Click **"Upload"** (the → arrow) on the bottom toolbar — or run from the
   project folder:

   ```bash
   cd esp32_firmware
   pio run -t upload --upload-port /dev/ttyUSB0    # <-- your port
   ```

   PlatformIO downloads the ESP32 toolchain + libraries on the **first** build
   (a few minutes). Later builds are fast.

6. Open the serial monitor (Plug icon on the bottom toolbar, or
   `pio device monitor`) at **115200 baud** to see boot logs.

### 4.5 What the OLED should show on boot

1. `Whisper` + `Connecting...`
2. `Ready!` + the ESP32's **IP address** (shown ~4 s)
3. The idle **Home** screen (mode, IP, and the button hints).

If it shows `WiFi failed`, your SSID/password in `config.h` is wrong — fix it
and re-flash.

---

## 5. Wiring

| Component | Pin | ESP32 GPIO | Notes |
|-----------|-----|------------|-------|
| OLED SDA | → | GPIO 21 | I2C data |
| OLED SCL | → | GPIO 22 | I2C clock |
| OLED VCC | → | 3.3V | **not** 5 V |
| OLED GND | → | GND | |
| Button 1 (HOME) | → | GPIO 32 | other leg to GND (uses internal pull-up) |
| Button 2 (MODE) | → | GPIO 33 | other leg to GND |
| Button 3 (HOLD-TO-SPEAK) | → | GPIO 25 | other leg to GND |
| LED anode | → | GPIO 26 | via ~220–330 Ω resistor, cathode → GND |
| Sound sensor A0 | → | GPIO 34 | analog in (input-only pin) |
| Sound sensor D0 | → | GPIO 35 | optional digital (unused by default) |
| Sound sensor VCC | → | 3.3V | |
| Sound sensor GND | → | GND | |

**Button wiring:** one leg to the GPIO, the other leg to GND. The firmware
enables each pin's internal pull-up, so no external resistor is needed.

**Why these pins?** GPIO 32/33/25/26 are general-purpose and safe. GPIO 34/35
are input-only pins (no internal pull-ups) — perfect for the sensor, which
drives them actively. None of these conflict with boot/strapping pins or I2C.

---

## 6. Run the demo

### 6.1 Start the laptop app

```bash
# in the laptop_app/ folder, with the venv activated:
python main.py
```

You should see something like:

```
Whisper Assistant — laptop app
STT backend : faster-whisper
Your laptop's local IP appears to be: 192.168.1.42
Serving on  : http://0.0.0.0:5000
Waiting for the ESP32 to connect...
```

Leave it running.

### 6.2 Power on the ESP32

Confirm the OLED shows `Ready!` + an IP address, then the Home screen.

### 6.3 Test each button

1. **Button 3 (hold-to-speak):** hold it, say *"What is the capital of
   France?"*, release. OLED: `Listening` → `Thinking` → the answer.
2. **Button 2 (mode):** press repeatedly — the OLED flashes
   `Home` → `Summarize` → `To-Do` → `Home`.
3. **Button 1 (home):** press — resets to Home/normal Q&A mode.
4. In **Summarize** or **To-Do** mode, hold button 3 and speak a longer
   sentence — the short version appears on the OLED, and the full structured
   version is appended to `laptop_app/notes.md`.

---

## 7. Demo checklist

- [ ] ESP32 powered, OLED shows `Ready!` + IP, then Home screen.
- [ ] Laptop app running (`python main.py`), prints its IP.
- [ ] ESP32 and laptop on the **same Wi-Fi network**.
- [ ] Hold button 3 → "Listening" → speak → release → "Thinking" → answer.
- [ ] Mode button cycles Home → Summarize → To-Do.
- [ ] Home button resets to Home mode.
- [ ] (If wired) sound sensor lights the LED on loud noise.

---

## 8. Troubleshooting

**ESP32 shows "WiFi failed"**
→ Wrong SSID/password in `config.h`. Fix, re-flash.

**OLED shows "Thinking..." forever, then "No response from laptop"**
→ The ESP32 couldn't reach the laptop. Check:
1. The laptop app is running.
2. `LAPTOP_IP` in `config.h` is the laptop's **local** IP (recheck `ipconfig`/`ip a`).
3. Both devices are on the **same network** (no guest-isolation, no VPN).
4. **Firewall:** Windows/macOS may block port 5000. On Windows, allow Python
   through the firewall ("Allow an app through Windows Firewall"), or
   temporarily disable the firewall for a private network. On macOS, allow
   incoming connections when prompted.

**Laptop never receives the button press**
→ Same checks as above. Also confirm the ESP32's serial monitor shows
`[HTTP] GET http://... -> 200`. A `-1`/`0` return code means connection
refused/timeout (laptop not reachable).

**"I didn't catch that" every time**
→ Microphone issue. Check your laptop's default mic works (record a note in
any OS app). If you see `[audio] start failed`, run
`python -c "import sounddevice as sd; print(sd.query_devices())"` to list
devices and pick the right one.

**"I didn't catch that" with `STT_BACKEND=openai`**
→ Check `OPENAI_API_KEY` is set in `.env`. If the laptop log says
`[stt] OPENAI_API_KEY is not set`, that (not the mic) is the cause.

**OLED shows "DeepSeek API key missing."**
→ `DEEPSEEK_API_KEY` in `laptop_app/.env` is empty or malformed. Paste the key
after `DEEPSEEK_API_KEY=` (no quotes, no spaces) and restart the app.

**OLED shows "LLM request failed"**
→ The DeepSeek call errored (bad/expired key, no internet, or rate limit).
Re-check the key and the laptop's internet; see the laptop terminal for `[llm]`
lines.

**Linux: "PortAudio library not found"**
→ Install the system library — `sudo apt-get install libportaudio2`
(Debian/Ubuntu) or `sudo dnf install portaudio` (Fedora) — then restart.

**First phrase is slow (30–60 s of "Thinking…")**
→ Normal: faster-whisper is downloading its model once. The OLED just stays on
"Thinking…" longer for that first phrase. Subsequent phrases are fast.
To skip this, set `STT_BACKEND=openai` (and add an `OPENAI_API_KEY`).

**OLED is blank / all pixels on**
→ Check SDA→21, SCL→22, and that VCC is on **3.3 V**. The firmware
auto-detects the I2C address (0x3C or 0x3D), so address is rarely the issue.
Check the serial monitor for `[oled] SSD1306 ready at 0x...`.

**Garbled characters on the OLED**
→ The OLED font is ASCII-only. Non-English responses are stripped on the
display but **are** preserved in full in `notes.md`.

---

## 9. How it works (short)

- **ESP32:** runs a small `WebServer` on port 80. Buttons are debounced in
  software; the hold-to-speak button sends `GET /start_listening?mode=…` on
  press and `GET /stop_listening` on release. It renders responses with
  Adafruit_SSD1306 + Adafruit_GFX, and vertically auto-scrolls anything that
  doesn't fit in 8 lines.
- **Laptop:** Flask serves `/start_listening` and `/stop_listening`. A
  `sounddevice` stream buffers 16 kHz mono audio while you hold the button.
  On release, a background thread runs STT → DeepSeek → sanitize → POST to
  the ESP32's `/display`. Summarize/To-Do modes also write a full version to
  `notes.md`.

---

## 10. Project layout

```
.
├── README.md
├── .gitignore
├── esp32_firmware/          # Part B — the "face"
│   ├── platformio.ini
│   └── src/
│       ├── config.h         # Wi-Fi + IP + pins + tunables (EDIT THIS)
│       └── main.cpp         # all firmware logic
└── laptop_app/              # Part A — the "brain"
    ├── main.py              # single entry point: python main.py
    ├── requirements.txt
    ├── .env.example         # copy to .env and add your key
    └── notes.md             # created at runtime (Summarize/To-Do logs)
```
