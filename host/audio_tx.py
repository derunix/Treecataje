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
import wave

DEFAULT_FREQ = 433.92  # MHz
DEFAULT_DEV = 2.5      # kHz narrowband FM deviation
DEFAULT_RATE = 8000    # Hz PCM sample rate (voice)
DEFAULT_OSR = 16       # sigma-delta oversampling ratio (bits per sample on device)


# --- TTS engine detection -----------------------------------------------------
def tts_engines():
    """Return the list of available TTS backends, best first."""
    found = []
    if shutil.which("espeak-ng"):
        found.append("espeak-ng")
    if shutil.which("espeak"):
        found.append("espeak")
    if shutil.which("pico2wave"):
        found.append("pico2wave")
    return found


def synth_tts(text, wav_out, voice="en", engine=None, speed=160):
    """Render `text` to a WAV file using an available TTS engine. Returns the
    engine used. Raises RuntimeError with install hints if none is available."""
    engines = tts_engines()
    if not engines:
        raise RuntimeError(
            "no TTS engine found. Install one, e.g.:\n"
            "  sudo apt install espeak-ng        # recommended, many voices\n"
            "  sudo apt install libttspico-utils # pico2wave\n"
            "or transmit an existing audio file with --file instead."
        )
    eng = engine or engines[0]
    if eng in ("espeak-ng", "espeak"):
        # -w writes a WAV; -s words/min; -v voice (e.g. en, ru, en+f3)
        subprocess.run([eng, "-v", voice, "-s", str(speed), "-w", wav_out, text],
                       check=True, capture_output=True)
    elif eng == "pico2wave":
        lang = voice if "-" in voice else "en-US"
        subprocess.run(["pico2wave", "-l", lang, "-w", wav_out, text],
                       check=True, capture_output=True)
    else:
        raise RuntimeError(f"unknown TTS engine {eng!r}")
    return eng


# --- audio -> raw u8 mono PCM -------------------------------------------------
def to_pcm_u8(src, raw_out, rate=DEFAULT_RATE):
    """Convert any audio file (wav/mp3/ogg/...) to headerless unsigned-8-bit mono
    PCM at `rate` Hz. Uses ffmpeg when present; falls back to stdlib wave+audioop
    for plain WAV input. Returns the byte count written."""
    if shutil.which("ffmpeg"):
        subprocess.run(
            ["ffmpeg", "-y", "-i", src, "-ac", "1", "-ar", str(int(rate)),
             "-f", "u8", "-acodec", "pcm_u8", raw_out],
            check=True, capture_output=True,
        )
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
             rate=DEFAULT_RATE, osr=DEFAULT_OSR, reps=1, voice="en",
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


def _main():
    import argparse
    from companion_proto import Companion
    ap = argparse.ArgumentParser(description="Analog FM voice/audio TX over CC1101")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--text", help="text to speak (TTS)")
    g.add_argument("--file", help="audio file to transmit (wav/mp3/ogg/...)")
    ap.add_argument("--port", default="/dev/ttyACM1")
    ap.add_argument("--token", default="")
    ap.add_argument("--freq", type=float, default=DEFAULT_FREQ, help="MHz")
    ap.add_argument("--dev", type=float, default=DEFAULT_DEV, help="deviation kHz")
    ap.add_argument("--rate", type=int, default=DEFAULT_RATE, help="PCM Hz")
    ap.add_argument("--osr", type=int, default=DEFAULT_OSR)
    ap.add_argument("--reps", type=int, default=1)
    ap.add_argument("--voice", default="en", help="TTS voice (e.g. en, ru, en+f3)")
    ap.add_argument("--engine", default=None, help="force TTS engine")
    args = ap.parse_args()

    dev = Companion(args.port)
    dev.hello(token=args.token)
    transmit(dev, source=args.file, text=args.text, freq=args.freq,
             dev_khz=args.dev, rate=args.rate, osr=args.osr, reps=args.reps,
             voice=args.voice, engine=args.engine)


if __name__ == "__main__":
    _main()
