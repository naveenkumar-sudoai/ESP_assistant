#!/usr/bin/env python3
"""
Whisper Assistant — laptop "brain".

A single-process app that:
  1. Runs a small Flask HTTP server that the ESP32 "face" calls into.
  2. Captures microphone audio while the ESP32's hold-to-speak button is held.
  3. Runs speech-to-text (faster-whisper locally, or the OpenAI Whisper API).
  4. Sends the recognized text + current mode to the DeepSeek API.
  5. Logs full responses to notes.md (Summarize / To-Do modes) and
  6. POSTs a short, OLED-friendly response back to the ESP32's /display.

Usage:
    python main.py
"""

import io
import os
import re
import socket
import threading
import time
import wave
from datetime import datetime

import numpy as np
import sounddevice as sd
import requests
from dotenv import load_dotenv
from flask import Flask, request, jsonify

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

ESP32_IP   = os.getenv("ESP32_IP", "").strip()   # optional: auto-learned from the ESP32 if empty
ESP32_PORT = _env_int("ESP32_PORT", 80)

LAPTOP_HOST = os.getenv("LAPTOP_HOST", "0.0.0.0")
LAPTOP_PORT = _env_int("LAPTOP_PORT", 5000)

SAMPLE_RATE = 16000
MAX_RECORD_SECONDS = 30.0

NOTES_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "notes.md")

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
        with self.lock:
            self.frames = []
        try:
            stream = sd.InputStream(
                samplerate=self.samplerate,
                channels=1,
                dtype="float32",
                callback=self._callback,
            )
            stream.start()
        except Exception:
            # Don't leave the recorder wedged: reset flags so a later press retries.
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
            print("[audio] no audio captured")
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
    print(f"[stt] faster-whisper -> {text!r}")
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
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm16.tobytes())
    buf.seek(0)
    buf.name = "audio.wav"

    resp = client.audio.transcriptions.create(model="whisper-1", file=buf)
    text = resp.text.strip()
    print(f"[stt] openai whisper -> {text!r}")
    return text

def transcribe(audio):
    if STT_BACKEND == "openai":
        return _transcribe_openai(audio)
    return _transcribe_faster_whisper(audio)


# ------------------------------------------------------------------- LLM --------
def call_deepseek(system_prompt, user_text, max_tokens=200):
    if not DEEPSEEK_API_KEY:
        print("[llm] DEEPSEEK_API_KEY is not set")
        return "DeepSeek API key missing."
    headers = {
        "Authorization": f"Bearer {DEEPSEEK_API_KEY}",
        "Content-Type": "application/json",
    }
    payload = {
        "model": DEEPSEEK_MODEL,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_text},
        ],
        "max_tokens": max_tokens,
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


def build_prompts(mode, text):
    short_rule = ("Keep the whole answer under ~90 characters. Plain ASCII text only, "
                  "no markdown, no emoji, no bullet symbols.")
    if mode == "summarize":
        short_sys = ("You are a concise summarizer. Summarize the user's input in one or "
                     "two short sentences. " + short_rule)
        full_sys = ("Summarize the following input in a clear, structured way. Use short "
                    "paragraphs and a few markdown bullet points if helpful.")
    elif mode == "todo":
        short_sys = ("You are a note taker. Extract at most 3 actionable to-do items from "
                     "the user's input. Put each item on its own line, plain text, no "
                     "bullets, no markdown. Keep each item under ~20 characters and the "
                     "whole answer under ~90 characters.")
        full_sys = ("Extract the actionable to-do items from the following input. Format as "
                    "a markdown bullet list, one item per line, and note any deadline, owner "
                    "or context in parentheses.")
    else:  # home
        short_sys = ("You are a friendly, concise voice assistant. Answer the user's question "
                     "directly and briefly. " + short_rule)
        full_sys = None
    return short_sys, full_sys


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


def append_note(mode, transcript, full):
    stamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    label = {"home": "Home", "summarize": "Summarize", "todo": "To-Do"}.get(mode, mode)
    block = (f"\n## [{label}] {stamp}\n\n"
             f"**You said:** {transcript}\n\n"
             f"**Result:**\n{full}\n")
    try:
        with open(NOTES_FILE, "a", encoding="utf-8") as f:
            f.write(block)
        print(f"[notes] appended to {NOTES_FILE}")
    except OSError as e:
        print(f"[notes] write error: {e}")


def process_utterance(audio, mode):
    text = transcribe(audio)
    if not text:
        return "I didn't catch that. Please try again."

    short_sys, full_sys = build_prompts(mode, text)

    short = call_deepseek(short_sys, text, max_tokens=120)
    short = sanitize_for_oled(short)
    if not short:
        short = "(empty response)"

    if full_sys is not None:
        full = call_deepseek(full_sys, text, max_tokens=500)
        append_note(mode, text, full)

    return short


# ------------------------------------------------------------ ESP32 messaging --
app = Flask(__name__)
recorder = AudioRecorder()

current_mode = "home"
esp_ip = ESP32_IP


def learn_esp_ip():
    global esp_ip
    if not esp_ip:
        addr = request.remote_addr
        if addr:
            esp_ip = addr
            print(f"[display] learned ESP32 IP: {esp_ip}")


def post_to_esp32(text):
    if not esp_ip:
        print("[display] ESP32 IP unknown; cannot send response")
        return False
    url = f"http://{esp_ip}:{ESP32_PORT}/display"
    try:
        r = requests.post(
            url,
            data=text.encode("utf-8"),
            headers={"Content-Type": "text/plain; charset=utf-8"},
            timeout=5,
        )
        print(f"[display] POST {url} -> {r.status_code}")
        return r.status_code == 200
    except requests.exceptions.RequestException as e:
        print(f"[display] POST failed: {e}")
        return False


def pipeline_worker(audio, mode):
    try:
        short = process_utterance(audio, mode)
        post_to_esp32(short)
    except Exception as e:
        print(f"[pipeline] error: {e}")
        post_to_esp32("Something went wrong.")


@app.route("/start_listening", methods=["GET", "POST"])
def start_listening():
    global current_mode
    m = request.args.get("mode", "home")
    if m in ("home", "summarize", "todo"):
        current_mode = m
    learn_esp_ip()
    try:
        recorder.start()
    except Exception as e:
        print(f"[audio] start failed: {e}")
        return jsonify({"status": "error", "error": str(e)}), 500
    return jsonify({"status": "ok", "mode": current_mode})


@app.route("/stop_listening", methods=["GET", "POST"])
def stop_listening():
    learn_esp_ip()
    audio = recorder.stop()
    threading.Thread(target=pipeline_worker, args=(audio, current_mode), daemon=True).start()
    return jsonify({"status": "ok", "thinking": True})


@app.route("/")
def index():
    return ("Whisper Assistant laptop app is running. "
            f"ESP32 IP = {esp_ip or 'unknown'}, mode = {current_mode}")


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


def check_audio():
    try:
        devices = sd.query_devices()
        n_in = sum(1 for d in devices if d.get("max_input_channels", 0) > 0)
        if n_in == 0:
            print("WARNING: no audio INPUT device found — recording will fail.")
        else:
            print(f"  Audio input : {n_in} device(s) available")
    except Exception as e:
        print("WARNING: audio unavailable — microphone / PortAudio problem.")
        print(f"  ({e})")
        print("  Linux: install PortAudio, e.g. 'sudo apt-get install libportaudio2'")
        print("  (Fedora: 'sudo dnf install portaudio')")


def main():
    print("=" * 60)
    print("  Whisper Assistant — laptop app")
    print("=" * 60)
    if not DEEPSEEK_API_KEY:
        print("WARNING: DEEPSEEK_API_KEY is not set in .env — LLM calls will fail.")
    print(f"  STT backend : {STT_BACKEND}")
    print(f"  Notes file  : {NOTES_FILE}")
    check_audio()
    print_laptop_ip()
    print(f"  Serving on  : http://{LAPTOP_HOST}:{LAPTOP_PORT}")
    print("  Waiting for the ESP32 to connect... (Ctrl+C to quit)")
    app.run(host=LAPTOP_HOST, port=LAPTOP_PORT, threaded=True, debug=False)


if __name__ == "__main__":
    main()
