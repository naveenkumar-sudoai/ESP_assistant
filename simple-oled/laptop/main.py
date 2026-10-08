#!/usr/bin/env python3
"""
Whisper Assistant (Simple) — laptop "brain".

OLED-only version. No buttons, no LED, no ESP32 mic. Flow:

  1. Press ENTER on the laptop to start talking (laptop microphone).
  2. Press ENTER again to finish.
  3. Speech is turned into text, then DeepSeek figures out what you want —
     a question, something to summarize, or a to-do list — and answers.
  4. The short answer is pushed to the ESP32 and shown on its OLED.

The ESP32 is just a wireless display: it registers with this app on boot, and
this app POSTs text to its /display endpoint.
"""

import io
import os
import re
import socket
import threading
import time

import numpy as np
import requests
from dotenv import load_dotenv
from flask import Flask, request

load_dotenv()


def _env_int(name, default):
    try:
        return int(os.getenv(name, str(default)))
    except (TypeError, ValueError):
        return default


# ------------------------------------------------------------------ config ----
DEEPSEEK_API_KEY = os.getenv("DEEPSEEK_API_KEY", "").strip()
DEEPSEEK_MODEL   = os.getenv("DEEPSEEK_MODEL", "deepseek-chat")

STT_BACKEND    = os.getenv("STT_BACKEND", "faster-whisper").strip().lower()  # faster-whisper | openai
WHISPER_MODEL  = os.getenv("WHISPER_MODEL", "base")
OPENAI_API_KEY = os.getenv("OPENAI_API_KEY", "").strip()

ESP32_IP   = os.getenv("ESP32_IP", "").strip()   # optional: auto-learned via /register if empty
ESP32_PORT = _env_int("ESP32_PORT", 80)

LAPTOP_HOST = os.getenv("LAPTOP_HOST", "0.0.0.0")
LAPTOP_PORT = _env_int("LAPTOP_PORT", 5000)

SAMPLE_RATE = 16000
MAX_RECORD_SECONDS = 20.0

DEEPSEEK_URL = "https://api.deepseek.com/chat/completions"


# ------------------------------------------------------------- audio capture ---
class AudioRecorder:
    """Records mono 16 kHz float32 audio into memory while `recording` is True."""

    def __init__(self, samplerate=SAMPLE_RATE, max_seconds=MAX_RECORD_SECONDS):
        self.samplerate = samplerate
        self.max_seconds = max_seconds
        self.lock = threading.Lock()
        self.frames = []
        self.stream = None
        self.started_at = 0.0
        self.recording = False

    def _callback(self, indata, frames, time_info, status):
        with self.lock:
            if self.recording and (time.time() - self.started_at) < self.max_seconds:
                self.frames.append(indata.copy())

    def start(self):
        if self.recording:
            return
        import sounddevice as sd
        with self.lock:
            self.frames = []
        try:
            stream = sd.InputStream(samplerate=self.samplerate, channels=1,
                                    dtype="float32", callback=self._callback)
            stream.start()
        except Exception:
            self.recording = False
            self.stream = None
            raise
        self.stream = stream
        self.started_at = time.time()
        self.recording = True
        print("[audio] recording started")

    def stop(self):
        if not self.recording:
            return None
        self.recording = False
        if self.stream is not None:
            self.stream.stop()
            self.stream.close()
            self.stream = None
        with self.lock:
            frames = self.frames
            self.frames = []
        if not frames:
            return None
        audio = np.concatenate(frames, axis=0)
        audio = np.squeeze(audio)
        print(f"[audio] captured {audio.shape[0] / self.samplerate:.2f}s of audio")
        return audio


# ------------------------------------------------------------- speech-to-text --
_whisper_model = None


def _faster_whisper_model():
    global _whisper_model
    if _whisper_model is None:
        from faster_whisper import WhisperModel
        print(f"[stt] loading faster-whisper model '{WHISPER_MODEL}' "
              "(first run downloads it — needs internet once)...")
        _whisper_model = WhisperModel(WHISPER_MODEL, device="cpu", compute_type="int8")
    return _whisper_model


def _transcribe_faster_whisper(audio):
    if audio is None or len(audio) < SAMPLE_RATE * 0.3:
        return ""
    model = _faster_whisper_model()
    segments, _info = model.transcribe(audio, beam_size=5)
    text = "".join(seg.text for seg in segments).strip()
    return text


def _transcribe_openai(audio):
    if audio is None or len(audio) < SAMPLE_RATE * 0.3:
        return ""
    if not OPENAI_API_KEY:
        print("[stt] OPENAI_API_KEY is not set; cannot use the openai backend")
        return ""
    from openai import OpenAI
    client = OpenAI(api_key=OPENAI_API_KEY)
    pcm16 = (np.clip(audio, -1.0, 1.0) * 32767.0).astype(np.int16)
    buf = io.BytesIO()
    import wave as _wave
    with _wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm16.tobytes())
    buf.seek(0)
    buf.name = "audio.wav"
    resp = client.audio.transcriptions.create(model="whisper-1", file=buf)
    return resp.text.strip()


def transcribe(audio):
    if STT_BACKEND == "openai":
        return _transcribe_openai(audio)
    return _transcribe_faster_whisper(audio)


# ---------------------------------------------------- DeepSeek: auto-classify ---
# One prompt that decides what the user wants, then answers. No mode buttons.
SYSTEM_PROMPT = (
    "You are a concise voice assistant. The user speaks a short message. Decide "
    "what they want and answer in ONE short response:\n"
    "- If they asked a question, answer it directly and briefly.\n"
    "- If they gave something to summarize, give a one-sentence summary.\n"
    "- If they listed tasks or things to do, output a short to-do list, one item "
    "per line.\n\n"
    "Rules: keep the whole answer under ~90 characters, plain ASCII text only, "
    "no markdown, no emoji, no bullet symbols."
)


def classify_and_answer(text):
    if not DEEPSEEK_API_KEY:
        return "DeepSeek API key missing."
    headers = {"Authorization": f"Bearer {DEEPSEEK_API_KEY}",
               "Content-Type": "application/json"}
    payload = {
        "model": DEEPSEEK_MODEL,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": text},
        ],
        "max_tokens": 200,
        "temperature": 0.7,
        "stream": False,
    }
    try:
        r = requests.post(DEEPSEEK_URL, headers=headers, json=payload, timeout=60)
        r.raise_for_status()
        data = r.json()
        return data["choices"][0]["message"]["content"].strip()
    except requests.exceptions.RequestException as e:
        print(f"[llm] request error: {e}")
        return "LLM request failed (see laptop log)."
    except Exception as e:
        print(f"[llm] unexpected error: {e}")
        return "LLM error."


def sanitize_for_oled(s):
    s = s.replace("**", "").replace("`", "").replace("#", "")
    s = s.replace("*", "").replace("_", " ")
    s = s.replace("—", "-").replace("–", "-")
    s = re.sub(r"[^\x20-\x7E\n]", "", s)   # ASCII printable + newline only
    s = re.sub(r"[ \t]+", " ", s)
    s = re.sub(r"\n[ \t]+", "\n", s)
    s = re.sub(r"\n{2,}", "\n", s)
    s = s.strip()
    if len(s) > 400:
        s = s[:400].rstrip() + "..."
    return s


# ------------------------------------------------------------ ESP32 messaging --
app = Flask(__name__)
esp_ip = ESP32_IP


@app.route("/register", methods=["GET", "POST"])
def register():
    global esp_ip
    if not esp_ip:
        esp_ip = request.remote_addr
        print(f"[display] learned ESP32 IP: {esp_ip}")
    return ("ok", 200)


@app.route("/")
def index():
    return (f"Whisper Assistant (simple) running. ESP32 IP = {esp_ip or 'unknown'}")


def post_to_esp32(text):
    if not esp_ip:
        print("[display] ESP32 IP unknown — is the ESP32 powered on and on the same Wi-Fi?")
        return False
    url = f"http://{esp_ip}:{ESP32_PORT}/display"
    try:
        r = requests.post(url, data=text.encode("utf-8"),
                          headers={"Content-Type": "text/plain; charset=utf-8"}, timeout=5)
        print(f"[display] POST {url} -> {r.status_code}")
        return r.status_code == 200
    except requests.exceptions.RequestException as e:
        print(f"[display] POST failed: {e}")
        return False


# --------------------------------------------------------------------- main ----
def print_laptop_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect(("8.8.8.8", 80))
            ip = s.getsockname()[0]
        except Exception:
            ip = "127.0.0.1"
        finally:
            s.close()
        print("=" * 60)
        print(f"  Your laptop's local IP appears to be: {ip}")
        print("  Put this IP as LAPTOP_IP in the ESP32's config.h")
        print("=" * 60)
    except Exception:
        pass


def main():
    print("=" * 60)
    print("  Whisper Assistant (Simple) — laptop app")
    print("=" * 60)
    if not DEEPSEEK_API_KEY:
        print("WARNING: DEEPSEEK_API_KEY is not set in .env — LLM calls will fail.")
    print(f"  STT backend : {STT_BACKEND}")

    # Start the Flask server (for /register) in the background.
    threading.Thread(target=lambda: app.run(host=LAPTOP_HOST, port=LAPTOP_PORT,
                                            threaded=True, debug=False),
                     daemon=True).start()
    time.sleep(1)
    print_laptop_ip()
    print(f"  Serving on  : http://{LAPTOP_HOST}:{LAPTOP_PORT}")
    print("  Waiting for the ESP32 to register...")
    print()
    print("  HOW TO USE:")
    print("    1. Press ENTER to start talking.")
    print("    2. Speak into the laptop microphone.")
    print("    3. Press ENTER again to finish.")
    print("    4. The answer appears on the OLED (and here).")
    print("    (Ctrl+C to quit)")
    print()

    recorder = AudioRecorder()
    while True:
        try:
            input(">> Press ENTER to start talking...")
        except (EOFError, KeyboardInterrupt):
            break

        try:
            recorder.start()
        except Exception as e:
            print(f"[audio] start failed: {e}")
            print("  Check your laptop microphone. Linux needs PortAudio "
                  "(sudo apt-get install libportaudio2 / dnf install portaudio).")
            continue

        print("Listening... press ENTER to finish.")
        stop_evt = threading.Event()
        threading.Thread(target=lambda: (input(), stop_evt.set()), daemon=True).start()
        stop_evt.wait(timeout=MAX_RECORD_SECONDS)   # auto-stop if they never press ENTER

        audio = recorder.stop()
        if audio is None or len(audio) < SAMPLE_RATE * 0.3:
            print("(didn't catch that — try again)")
            continue

        text = transcribe(audio)
        if not text:
            print("(didn't catch that — try again)")
            continue
        print(f"\n  You said : {text}")

        answer = sanitize_for_oled(classify_and_answer(text))
        print(f"  Answer   : {answer}\n")
        post_to_esp32(answer)


if __name__ == "__main__":
    main()
