#!/usr/bin/env python3
"""
Play (or open) the last ESP32-mic recording saved to debug_audio/.

The laptop app writes `debug_audio/last.wav` whenever AUDIO_SOURCE=esp32 and
DEBUG_AUDIO=true. Run this to listen back and check mic quality / gain / wiring:

    python tools/play_last_audio.py

It tries `sounddevice` playback first (best experience); if that's unavailable it
opens the file with the OS default audio player instead.
"""

import os
import sys
import wave

import numpy as np


def find_wav():
    base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # laptop_app/
    debug_dir = os.path.join(base, "debug_audio")

    last = os.path.join(debug_dir, "last.wav")
    if os.path.isfile(last):
        return last

    # Fall back to the newest rec_*.wav if last.wav is missing.
    if os.path.isdir(debug_dir):
        recs = sorted(
            os.path.join(debug_dir, f)
            for f in os.listdir(debug_dir)
            if f.endswith(".wav")
        )
        if recs:
            return recs[-1]
    return None


def play(path):
    print(f"Playing: {path}")

    # Preferred: sounddevice playback (cross-platform, already a dependency).
    try:
        import sounddevice as sd
        with wave.open(path, "rb") as w:
            nch, sampwidth, rate = w.getnchannels(), w.getsampwidth(), w.getframerate()
            data = w.readframes(w.getnframes())
        if sampwidth != 2:
            raise ValueError(f"expected 16-bit WAV, got {sampwidth * 8}-bit")
        audio = np.frombuffer(data, dtype=np.int16).astype(np.float32) / 32768.0
        if nch != 1:
            audio = audio.reshape(-1, nch)
        print(f"  ({rate} Hz, {nch} ch, {audio.shape[0] / rate:.2f}s)")
        sd.play(audio, rate)
        sd.wait()
        return
    except Exception as e:  # noqa: BLE001 — fall back to the OS player
        print(f"  (sounddevice playback unavailable: {e})")
        print("  Opening with the OS default player instead...")

    try:
        if sys.platform == "win32":
            os.startfile(path)  # noqa: S606
        elif sys.platform == "darwin":
            os.system(f'open "{path}"')
        else:
            os.system(f'xdg-open "{path}"')
    except Exception as e:  # noqa: BLE001
        print(f"Could not open {path}: {e}")
        sys.exit(1)


if __name__ == "__main__":
    p = find_wav()
    if not p:
        print("No debug audio found.")
        print("Record something first with AUDIO_SOURCE=esp32 and DEBUG_AUDIO=true.")
        sys.exit(1)
    play(p)
