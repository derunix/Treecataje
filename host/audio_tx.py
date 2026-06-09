#!/usr/bin/env python3
"""Analog FM voice / audio transmission over the CC1101 (~433/443 MHz).

The CC1101 has no native analog-FM audio path. The firmware synthesises one: it
oversamples 8-bit unsigned mono PCM into a 1-bit sigma-delta stream and keys GDO0
in 2-FSK, so the carrier hops +/-deviation per bit and an analog FM receiver's
discriminator integrates the bit density back into audio. Narrowband deviation
(~2.5 kHz) matches PMR / analog walkie-talkies.

This host module turns text (TTS) or an audio file into that raw PCM, uploads it,
and triggers playback:

    from companion_proto import Companion
    import audio_tx
    dev = Companion("/dev/ttyACM1"); dev.hello()
    audio_tx.transmit(dev, text="break break, this is a test", freq=443.0)
    audio_tx.transmit(dev, source="clip.mp3", freq=433.92, reps=2)

CLI:
    python3 audio_tx.py --text "hello on four three three" --freq 433.92
    python3 audio_tx.py --file message.wav --freq 443.0 --dev 2.5 --reps 3

AUTHORIZED USE ONLY: transmit on bands/frequencies you are licensed/permitted to
use, to your own radios. RF transmission is regulated.
"""
import os
import shutil
import subprocess
import tempfile
import time
import wave

DEFAULT_FREQ = 433.92  # MHz
DEFAULT_DEV = 4.0      # kHz FM deviation (narrowband voice; 4-4.5 = intelligible)
DEFAULT_RATE = 8000    # Hz PCM sample rate (voice)
DEFAULT_OSR = 32       # sigma-delta oversampling ratio (higher = less quant. noise)


# RHVoice Russian voice profiles (high quality, far better than espeak for ru)
RHVOICE_RU = ("elena", "aleksandr", "anna", "irina", "artemiy", "pavel")
DEFAULT_RU_VOICE = "elena"


# --- TTS engine detection -----------------------------------------------------
def tts_engines():
    """Return the list of available TTS backends, best first."""
    found = []
    if shutil.which("RHVoice-test"):
        found.append("rhvoice")  # best quality, multi-language incl. Russian
    if shutil.which("espeak-ng"):
        found.append("espeak-ng")
    if shutil.which("espeak"):
        found.append("espeak")
    if shutil.which("pico2wave"):
        found.append("pico2wave")
    return found


def list_voices():
    """Return [(label, value)] of selectable TTS voices across the available
    engines, for populating UI pickers. `value` is what to pass as `voice`."""
    engines = tts_engines()
    out = [("Auto (по тексту)", "auto")]
    if "rhvoice" in engines:
        vdir = "/usr/share/RHVoice/voices"
        ru = [v for v in RHVOICE_RU if os.path.isdir(os.path.join(vdir, v))]
        for v in (ru or list(RHVOICE_RU)):
            out.append((f"Русский — {v} (RHVoice)", v))
    if "espeak-ng" in engines or "espeak" in engines:
        out += [("English — en", "en"),
                ("English ♀ — en+f3", "en+f3"),
                ("English ♂ — en+m3", "en+m3"),
                ("Deutsch — de", "de"),
                ("Français — fr", "fr"),
                ("Español — es", "es")]
        if "rhvoice" not in engines:
            out.insert(1, ("Русский — ru (espeak)", "ru"))
    return out


def detect_voice(text):
    """Pick a TTS voice from the script of `text`. Cyrillic -> Russian, else
    English. Keeps Russian (and other languages) working without the caller
    having to specify a voice."""
    for ch in text:
        if "Ѐ" <= ch <= "ӿ":  # Cyrillic block
            return "ru"
    return "en"


def synth_tts(text, wav_out, voice="auto", engine=None, speed=160):
    """Render `text` to a WAV file using an available TTS engine. voice='auto'
    detects the language from the text (Cyrillic -> ru). For Russian, RHVoice is
    strongly preferred — espeak-ng's Russian is too robotic to survive the
    narrowband FM link. Returns "engine:voice". Raises if no engine is available."""
    if not voice or voice == "auto":
        voice = detect_voice(text)
    engines = tts_engines()
    if not engines:
        raise RuntimeError(
            "no TTS engine found. Install one, e.g.:\n"
            "  sudo apt install rhvoice rhvoice-russian  # best, incl. Russian\n"
            "  sudo apt install espeak-ng                # many voices\n"
            "or transmit an existing audio file with --file instead."
        )
    is_ru = (voice == "ru") or (voice in RHVOICE_RU)
    # prefer RHVoice for Russian (and honour an explicit RHVoice profile name)
    if "rhvoice" in engines and (is_ru or (engine == "rhvoice")):
        prof = voice if voice in RHVOICE_RU else DEFAULT_RU_VOICE
        subprocess.run(["RHVoice-test", "-p", prof, "-o", wav_out],
                       input=text.encode("utf-8"), check=True, capture_output=True)
        return "rhvoice:" + prof
    eng = engine or (engines[0] if engines[0] != "rhvoice" else
                     (engines[1] if len(engines) > 1 else "rhvoice"))
    if eng == "rhvoice":
        prof = voice if voice in RHVOICE_RU else DEFAULT_RU_VOICE
        subprocess.run(["RHVoice-test", "-p", prof, "-o", wav_out],
                       input=text.encode("utf-8"), check=True, capture_output=True)
        return "rhvoice:" + prof
    if eng in ("espeak-ng", "espeak"):
        # -w writes a WAV; -s words/min; -v voice (e.g. en, ru, en+f3)
        subprocess.run([eng, "-v", voice, "-s", str(speed), "-w", wav_out, text],
                       check=True, capture_output=True)
        return eng + ":" + voice
    if eng == "pico2wave":
        lang = voice if "-" in voice else "en-US"
        subprocess.run(["pico2wave", "-l", lang, "-w", wav_out, text],
                       check=True, capture_output=True)
        return "pico2wave:" + lang
    raise RuntimeError(f"unknown TTS engine {eng!r}")


# --- audio -> raw u8 mono PCM -------------------------------------------------
def to_pcm_u8(src, raw_out, rate=DEFAULT_RATE, condition=True):
    """Convert any audio file (wav/mp3/ogg/...) to headerless unsigned-8-bit mono
    PCM at `rate` Hz. Uses ffmpeg when present; falls back to stdlib wave+audioop
    for plain WAV input. Returns the byte count written.

    With condition=True (recommended for the narrowband FM link) the audio is
    band-limited to the ~300-3400 Hz voice band and AGC-normalised so it uses the
    full PCM range — this maximises FM modulation depth and keeps energy inside the
    receiver's passband, which is what makes speech intelligible over the
    sigma-delta link (an un-normalised TTS clip modulates too shallowly)."""
    if shutil.which("ffmpeg"):
        cmd = ["ffmpeg", "-y", "-i", src, "-ac", "1", "-ar", str(int(rate))]
        if condition:
            # band-pass to the voice band, then dynamic-range normalise (AGC)
            cmd += ["-af", "highpass=f=300,lowpass=f=3400,"
                    "dynaudnorm=f=150:g=15:p=0.9,alimiter=limit=0.95"]
        cmd += ["-f", "u8", "-acodec", "pcm_u8", raw_out]
        subprocess.run(cmd, check=True, capture_output=True)
        return os.path.getsize(raw_out)
    return _wav_to_pcm_u8_stdlib(src, raw_out, rate)


def _wav_to_pcm_u8_stdlib(src, raw_out, rate):
    """ffmpeg-free path: read a PCM WAV, downmix to mono, resample, to u8."""
    import audioop
    with wave.open(src, "rb") as w:
        ch, width, fr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        data = w.readframes(n)
    if ch > 1:
        data = audioop.tomono(data, width, 0.5, 0.5)
    if width != 2:
        data = audioop.lin2lin(data, width, 2)
        width = 2
    if fr != rate:
        data, _ = audioop.ratecv(data, width, 1, fr, int(rate), None)
    # 16-bit signed -> 8-bit unsigned (bias 128)
    data = audioop.lin2lin(data, 2, 1)
    data = audioop.bias(data, 1, 128)
    with open(raw_out, "wb") as f:
        f.write(data)
    return len(data)


# --- orchestration ------------------------------------------------------------
def transmit(dev, source=None, text=None, freq=DEFAULT_FREQ, dev_khz=DEFAULT_DEV,
             rate=DEFAULT_RATE, osr=DEFAULT_OSR, reps=1, voice="auto",
             engine=None, remote_path="/audio_tx.raw", keep=False, log=print):
    """Synthesise (text) or convert (source file) audio, upload it, and transmit
    it as analog FM over the CC1101. Exactly one of `source`/`text` is required.
    Returns dict(bytes, secs, engine, response)."""
    if not source and not text:
        raise ValueError("transmit() needs either source=<file> or text=<str>")
    tmp = tempfile.mkdtemp(prefix="cc1101_audio_")
    used_engine = None
    try:
        if text:
            wav = os.path.join(tmp, "tts.wav")
            used_engine = synth_tts(text, wav, voice=voice, engine=engine)
            log(f"[tts] {used_engine}: {text!r}")
            src = wav
        else:
            src = source
            log(f"[audio] source: {src}")
        raw = os.path.join(tmp, "audio.raw")
        nbytes = to_pcm_u8(src, raw, rate=rate)
        secs = nbytes / float(rate)
        log(f"[pcm] {nbytes} bytes = {secs:.1f}s @ {rate} Hz mono u8")
        log(f"[tx] upload -> {remote_path}, then FM {freq:g} MHz dev={dev_khz:g} kHz "
            f"osr={osr} reps={reps} (~{secs*reps:.1f}s on air)")
        r = dev.audio_tx(raw, freq=freq, dev=dev_khz, rate=rate, osr=osr,
                         reps=reps, remote_path=remote_path)
        ok = getattr(r, "ok", False)
        log(f"[tx] {'done' if ok else 'failed'}: {'; '.join(getattr(r, 'lines', []))}"
            f"{'' if ok else ' ' + str(getattr(r, 'error', ''))}")
        return {"bytes": nbytes, "secs": secs, "engine": used_engine,
                "ok": ok, "response": r}
    finally:
        if not keep:
            shutil.rmtree(tmp, ignore_errors=True)


# --- RX: decode audio from the captured 1-bit demod stream --------------------
# Capture and decode are deliberately decoupled: the raw 1-bit GDO0 stream is
# saved verbatim (+ a JSON sidecar) so it can be re-decoded offline with different
# parameters to improve quality, without re-capturing or touching the radio.
RX_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "captures")


def decode_capture(packed, nbits, in_rate, wav_out, out_rate=8000,
                   cutoff=3400, hpf=200, gain="auto"):
    """Decode the packed 1-bit FM-demod stream into a WAV. The bit density tracks
    instantaneous frequency (the audio), so we map bits to +/-1, anti-alias
    low-pass (windowed-sinc FIR) + decimate to out_rate, high-pass to drop the FM
    DC/centre drift, and normalise. cutoff/hpf/out_rate are the quality knobs —
    re-run on a saved raw capture to tune. Returns the WAV sample rate."""
    import numpy as np
    bits = np.unpackbits(np.frombuffer(packed, dtype=np.uint8))[:nbits]
    if bits.size == 0:
        raise ValueError("empty capture")
    s = bits.astype(np.float32) * 2.0 - 1.0

    # anti-alias low-pass FIR at `cutoff`, then decimate to out_rate
    fc = min(cutoff, out_rate * 0.45) / (in_rate / 2.0)
    ntaps = 201
    n = np.arange(ntaps) - (ntaps - 1) / 2.0
    h = np.sinc(2 * fc * n) * np.hamming(ntaps)
    h /= h.sum()
    filt = np.convolve(s, h, mode="same")
    step = in_rate / float(out_rate)
    idx = (np.arange(int(len(filt) / step)) * step).astype(int)
    a = filt[idx]

    # high-pass (1st-order) to remove the FM centre offset / slow drift
    if hpf > 0 and a.size > 1:
        rc = 1.0 / (2 * 3.14159 * hpf)
        alpha = rc / (rc + 1.0 / out_rate)
        y = np.empty_like(a)
        y[0] = 0.0
        prev_x = a[0]
        prev_y = 0.0
        # vectorised-ish single-pole HPF
        for i in range(1, a.size):
            prev_y = alpha * (prev_y + a[i] - prev_x)
            prev_x = a[i]
            y[i] = prev_y
        a = y

    a = a - a.mean()
    peak = float(np.max(np.abs(a))) or 1.0
    g = (30000.0 / peak) if gain == "auto" else float(gain)
    pcm = np.clip(a * g, -32768, 32767).astype("<i2").tobytes()
    with wave.open(wav_out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(int(out_rate))
        w.writeframes(pcm)
    return int(out_rate)


def decode_file(raw_path, out_wav=None, in_rate=None, **opts):
    """Re-decode a saved raw capture (offline, tunable). Reads the .json sidecar
    for rate/bits if present. Returns the WAV path."""
    import json
    with open(raw_path, "rb") as f:
        packed = f.read()
    meta = {}
    side = raw_path + ".json"
    if os.path.exists(side):
        with open(side) as f:
            meta = json.load(f)
    in_rate = in_rate or meta.get("rate", 100000)
    nbits = meta.get("bits", len(packed) * 8)
    if out_wav is None:
        out_wav = os.path.splitext(raw_path)[0] + ".wav"
    decode_capture(packed, nbits, in_rate, out_wav, **opts)
    return out_wav


def _play(wav_path):
    """Play a WAV through whatever audio player is available."""
    for player in (["ffplay", "-autoexit", "-nodisp", "-loglevel", "quiet", wav_path],
                   ["aplay", "-q", wav_path], ["paplay", wav_path]):
        if shutil.which(player[0]):
            try:
                subprocess.run(player, check=False)
                return player[0]
            except Exception:  # noqa: BLE001
                continue
    return None


def record(dev, freq=DEFAULT_FREQ, wait=30, secs=20, rssi=-90, rate=100000,
           hold=400, out_wav=None, play=True, remote_path="/audio_rx.bin",
           stamp=None, log=print):
    """Carrier-triggered receive: arm the device on `freq`, wait for a carrier,
    capture the demodulated bitstream, fetch it, SAVE the raw 1-bit capture (+ a
    JSON sidecar so it can be re-decoded offline), then decode a WAV and optionally
    play it. Returns dict(raw, wav, secs, rate, bits) or None if no carrier.
    `stamp` is an optional filename timestamp string (Date is unavailable here)."""
    import re
    import json
    log(f"[rx] arming {freq:g} MHz, waiting up to {wait}s for a carrier "
        f"(rssi>={rssi} dBm)…")
    r = dev.audio_rx(freq=freq, wait=wait, secs=secs, rssi=rssi, rate=rate,
                     hold=hold, remote_path=remote_path)
    lines = " ".join(getattr(r, "lines", []))
    if "no carrier" in lines:
        log("[rx] no carrier seen in the window — nothing captured")
        return None
    m = re.search(r"bits=(\d+)\s+rate=(\d+)\s+bytes=(\d+)\s+ms=(\d+)", lines)
    if not m:
        log(f"[rx] unexpected response: {lines or getattr(r, 'error', '')}")
        return None
    nbits, in_rate, nbytes, ms = (int(m.group(i)) for i in range(1, 5))
    log(f"[rx] captured {ms/1000:.1f}s ({nbits} bits @ {in_rate} Hz) → fetching…")

    os.makedirs(RX_DIR, exist_ok=True)
    tag = stamp or time.strftime("%Y%m%d-%H%M%S")
    base = os.path.join(RX_DIR, f"rx_{freq:g}MHz_{tag}")
    raw_path = base + ".bin"
    dev.file_get(remote_path, raw_path, timeout=max(30, nbytes / 800))
    with open(raw_path + ".json", "w") as f:
        json.dump({"rate": in_rate, "bits": nbits, "freq": freq,
                   "secs": ms / 1000.0, "rssi_thr": rssi}, f)
    with open(raw_path, "rb") as f:
        packed = f.read()
    if out_wav is None:
        out_wav = base + ".wav"
    ar = decode_capture(packed, nbits, in_rate, out_wav)
    log(f"[rx] raw → {raw_path}  (re-decode offline: audio_tx.decode_file)")
    log(f"[rx] decoded → {out_wav} ({ar} Hz mono)")
    if play:
        p = _play(out_wav)
        log(f"[rx] played via {p}" if p else "[rx] no audio player found")
    return {"raw": raw_path, "wav": out_wav, "secs": ms / 1000.0, "rate": ar,
            "bits": nbits}


def _main():
    import argparse
    from companion_proto import Companion
    ap = argparse.ArgumentParser(description="Analog FM voice/audio TX over CC1101")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--text", help="text to speak (TTS) and transmit")
    g.add_argument("--file", help="audio file to transmit (wav/mp3/ogg/...)")
    g.add_argument("--rx", action="store_true",
                   help="receive: arm on a carrier, record, reconstruct + play")
    g.add_argument("--decode", metavar="RAW.bin",
                   help="offline: re-decode a saved raw capture (no device needed)")
    ap.add_argument("--port", default="/dev/ttyACM1")
    ap.add_argument("--token", default="")
    ap.add_argument("--freq", type=float, default=DEFAULT_FREQ, help="MHz")
    ap.add_argument("--dev", type=float, default=DEFAULT_DEV, help="deviation kHz")
    ap.add_argument("--rate", type=int, default=DEFAULT_RATE, help="PCM Hz (tx)")
    ap.add_argument("--osr", type=int, default=DEFAULT_OSR)
    ap.add_argument("--reps", type=int, default=1)
    ap.add_argument("--voice", default="auto", help="TTS voice (auto|en|ru|en+f3…)")
    ap.add_argument("--engine", default=None, help="force TTS engine")
    # rx options
    ap.add_argument("--wait", type=int, default=30, help="rx: seconds to wait for a carrier")
    ap.add_argument("--secs", type=int, default=20, help="rx: max record seconds")
    ap.add_argument("--rssi", type=int, default=-90, help="rx: carrier threshold dBm")
    ap.add_argument("--rxrate", type=int, default=100000, help="rx: GDO0 sample Hz")
    ap.add_argument("--out", default=None, help="rx/decode: WAV output path")
    # decode quality knobs (offline re-decode)
    ap.add_argument("--cutoff", type=int, default=3400, help="decode: low-pass Hz")
    ap.add_argument("--hpf", type=int, default=200, help="decode: high-pass Hz")
    ap.add_argument("--outrate", type=int, default=8000, help="decode: WAV Hz")
    args = ap.parse_args()

    if args.decode:  # offline, no device
        wav = decode_file(args.decode, out_wav=args.out, out_rate=args.outrate,
                          cutoff=args.cutoff, hpf=args.hpf)
        print("decoded ->", wav)
        _play(wav)
        return

    dev = Companion(args.port)
    dev.hello(token=args.token)
    if args.rx:
        record(dev, freq=args.freq, wait=args.wait, secs=args.secs, rssi=args.rssi,
               rate=args.rxrate, out_wav=args.out)
    else:
        transmit(dev, source=args.file, text=args.text, freq=args.freq,
                 dev_khz=args.dev, rate=args.rate, osr=args.osr, reps=args.reps,
                 voice=args.voice, engine=args.engine)


if __name__ == "__main__":
    _main()
