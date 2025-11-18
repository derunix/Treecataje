#include "audio.h"
#include "AudioFileSourceFunction.h"
#include "AudioGeneratorMIDI.h"
#include "AudioGeneratorWAV.h"
#include "AudioGeneratorAAC.h"
#include "AudioGeneratorFLAC.h"
#include "AudioOutputI2SNoDAC.h"
#include "core/mykeyboard.h"
#include <ESP8266Audio.h>
#include <ESP8266SAM.h>
#include "driver/i2s.h"
#include <functional>
#include <algorithm>
#include <cstring>

#ifdef ESP32
#include <esp_idf_version.h>
#endif

static bool configureI2SPinout(AudioOutputI2S *output) {
#if defined(BCLK) && defined(WCLK) && defined(DOUT)
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
#ifdef MCLK
    return output->SetPinout(BCLK, WCLK, DOUT, MCLK);
#else
    return output->SetPinout(BCLK, WCLK, DOUT);
#endif
#else
    return output->SetPinout(BCLK, WCLK, DOUT);
#endif
#else
    log_w("Skipping audio output configuration: I2S pins not defined");
    return false;
#endif
}

#if defined(HAS_NS4168_SPKR)
static bool audioI2SActive = false;
static const uint32_t DEFAULT_WAV_SR = 16000;

namespace {
void clearInputFlags() {
    NextPress = false;
    PrevPress = false;
    UpPress = false;
    DownPress = false;
    SelPress = false;
    EscPress = false;
    AnyKeyPress = false;
    SerialCmdPress = false;
    LongPress = false;
}

void drainInputNoise() {
    const int cycles = 4;
    for (int i = 0; i < cycles; i++) {
        clearInputFlags();
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
    clearInputFlags();
}
} // namespace

static bool rewriteSimpleWavHeader(FS *fs, const String &filepath, uint32_t sampleRate, size_t fileSize) {
    if (!fs) return false;
    if (fileSize < 44) return false;
    size_t dataSize = fileSize - 44;

    uint16_t numChannels = 1;
    uint16_t bitsPerSample = 16;
    uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);
    uint16_t blockAlign = numChannels * (bitsPerSample / 8);
    uint32_t fileSizeMinus8 = dataSize + 36;

    uint8_t hdr[44] = {0};
    memcpy(hdr, "RIFF", 4);
    hdr[4] = fileSizeMinus8 & 0xFF;
    hdr[5] = (fileSizeMinus8 >> 8) & 0xFF;
    hdr[6] = (fileSizeMinus8 >> 16) & 0xFF;
    hdr[7] = (fileSizeMinus8 >> 24) & 0xFF;
    memcpy(&hdr[8], "WAVE", 4);
    memcpy(&hdr[12], "fmt ", 4);
    hdr[16] = 16;
    hdr[20] = 1;
    hdr[22] = numChannels;
    hdr[24] = sampleRate & 0xFF;
    hdr[25] = (sampleRate >> 8) & 0xFF;
    hdr[26] = (sampleRate >> 16) & 0xFF;
    hdr[27] = (sampleRate >> 24) & 0xFF;
    hdr[28] = byteRate & 0xFF;
    hdr[29] = (byteRate >> 8) & 0xFF;
    hdr[30] = (byteRate >> 16) & 0xFF;
    hdr[31] = (byteRate >> 24) & 0xFF;
    hdr[32] = blockAlign & 0xFF;
    hdr[33] = (blockAlign >> 8) & 0xFF;
    hdr[34] = bitsPerSample & 0xFF;
    hdr[35] = (bitsPerSample >> 8) & 0xFF;
    memcpy(&hdr[36], "data", 4);
    hdr[40] = dataSize & 0xFF;
    hdr[41] = (dataSize >> 8) & 0xFF;
    hdr[42] = (dataSize >> 16) & 0xFF;
    hdr[43] = (dataSize >> 24) & 0xFF;

    File w = fs->open(filepath, "r+");
    if (!w) return false;
    size_t written = w.write(hdr, sizeof(hdr));
    w.flush();
    w.close();
    return written == sizeof(hdr);
}

bool playAudioFile(FS *fs, String filepath, AudioProgressCb progressCb, size_t startPos, bool *stoppedByCb) {
    if (!bruceConfig.soundEnabled) return false;

    // Clear stale key state so playback is not aborted immediately
    drainInputNoise();

    // Make sure no stale I2S driver instance is hanging around
    if (audioI2SActive) {
        i2s_driver_uninstall(I2S_NUM_0);
        audioI2SActive = false;
    }

    AudioFileSource *source = new AudioFileSourceFS(*fs, filepath.c_str());
    if (!source) return false;

    if (filepath.endsWith(".wav") && fs) {
        File chk = fs->open(filepath, FILE_READ);
        size_t sz = 0;
        if (chk) {
            sz = chk.size();
            chk.close();
        }
        if (sz >= 44) rewriteSimpleWavHeader(fs, filepath, DEFAULT_WAV_SR, sz);
    }

    if (startPos > 0) source->seek(startPos, SEEK_SET);

    AudioOutputI2S *audioout = new AudioOutputI2S();
    if (!audioout || !configureI2SPinout(audioout)) {
        delete audioout;
        delete source;
        return false;
    }

    // set volume, derived from https://github.com/earlephilhower/ESP8266Audio/blob/master/examples/WebRadio/WebRadio.ino
    audioout->SetGain(((float)bruceConfig.soundVolume) / 100.0);

    AudioGenerator *generator = NULL;

    // switch on extension
    filepath.toLowerCase(); // case-insensitive match
    if (filepath.endsWith(".txt") || filepath.endsWith(".rtttl")) generator = new AudioGeneratorRTTTL();
    if (filepath.endsWith(".wav")) generator = new AudioGeneratorWAV();
    if (filepath.endsWith(".mod")) generator = new AudioGeneratorMOD();
    if (filepath.endsWith(".opus")) generator = new AudioGeneratorOpus();
    if (filepath.endsWith(".aac")) generator = new AudioGeneratorAAC();
    if (filepath.endsWith(".flac")) generator = new AudioGeneratorFLAC();
    // OGG Vorbis is not supported https://github.com/earlephilhower/ESP8266Audio/issues/84
    if (filepath.endsWith(".mp3")) {
        generator = new AudioGeneratorMP3();
        source = new AudioFileSourceID3(source);
    }
    /* 2FIX: compilation issues
    if(filepath.endsWith(".mid"))  {
      // need to load a soundfont
      AudioFileSource* sf2 = NULL;
      if(setupSdCard()) sf2 = new AudioFileSourceFS(SD, "1mgm.sf2");  // TODO: make configurable
      if(!sf2) sf2 = new AudioFileSourceLittleFS(LittleFS, "1mgm.sf2");  // TODO: make configurable
      if(!sf2) return false;  // a soundfount was not found
      AudioGeneratorMIDI* midi = new AudioGeneratorMIDI();
      midi->SetSoundfont(sf2);
      generator = midi;
    } */
    if (generator && source && audioout) {
        Serial.println("Start audio");
        if (!generator->begin(source, audioout)) {
            Serial.println("Audio begin failed");
            delete generator;
            delete source;
            delete audioout;
            if (audioI2SActive) {
                i2s_driver_uninstall(I2S_NUM_0);
                audioI2SActive = false;
            }
            return false;
        }
        audioI2SActive = true;
        unsigned long lastCb = 0;
        bool cbStopped = false;
        while (generator->isRunning()) {
            if (!generator->loop()) generator->stop();
            if (progressCb) {
                unsigned long now = millis();
                if (now - lastCb > 100) {
                    size_t pos = source->getPos();
                    size_t size = source->getSize();
                    if (!progressCb(pos, size, audioout)) {
                        generator->stop();
                        cbStopped = true;
                        break;
                    }
                    lastCb = now;
                }
            }
            // yield a tiny amount to keep WDT happy and allow UI updates
            vTaskDelay(1);
        }
        audioout->stop();
        source->close();
        Serial.println("Stop audio");

        delete generator;
        delete source;
        delete audioout;
        if (audioI2SActive) {
            i2s_driver_uninstall(I2S_NUM_0);
            audioI2SActive = false;
        }

        // Drop any input that may have been latched while playing audio
        drainInputNoise();

        if (stoppedByCb) *stoppedByCb = cbStopped;
        return true;
    }
    // else
    if (audioI2SActive) {
        i2s_driver_uninstall(I2S_NUM_0);
        audioI2SActive = false;
    }
    drainInputNoise();
    return false; // init error
}

bool playAudioFile(FS *fs, String filepath) { return playAudioFile(fs, filepath, nullptr, 0, nullptr); }

bool playAudioRTTTLString(String song) {
    if (!bruceConfig.soundEnabled) return false;

    // derived from
    // https://github.com/earlephilhower/ESP8266Audio/blob/master/examples/PlayRTTTLToI2SDAC/PlayRTTTLToI2SDAC.ino

    song.trim();
    if (song == "") return false;

    AudioOutputI2S *audioout = new AudioOutputI2S();
    if (!audioout || !configureI2SPinout(audioout)) {
        delete audioout;
        return false;
    }

    AudioGenerator *generator = new AudioGeneratorRTTTL();

    AudioFileSource *source = new AudioFileSourcePROGMEM(song.c_str(), song.length());

    if (generator && source && audioout) {
        Serial.println("Start audio");
        generator->begin(source, audioout);
        while (generator->isRunning()) {
            if (!generator->loop() || check(AnyKeyPress)) generator->stop();
        }
        audioout->stop();
        source->close();
        Serial.println("Stop audio");

        delete generator;
        delete source;
        delete audioout;

        return true;
    }
    // else
    return false; // init error
}

bool tts(String text) {
    if (!bruceConfig.soundEnabled) return false;

    text.trim();
    if (text == "") return false;

    AudioOutputI2S *audioout = new AudioOutputI2S();
    if (!audioout || !configureI2SPinout(audioout)) {
        delete audioout;
        return false;
    }

    // https://github.com/earlephilhower/ESP8266SAM/blob/master/examples/Speak/Speak.ino
    audioout->begin();
    ESP8266SAM *sam = new ESP8266SAM;
    sam->Say(audioout, text.c_str());
    delete sam;
    return true;
}

bool isAudioFile(String filepath) {

    return filepath.endsWith(".opus") || filepath.endsWith(".rtttl") || filepath.endsWith(".wav") ||
           filepath.endsWith(".mod") || filepath.endsWith(".mp3");
}

void playTone(unsigned int frequency, unsigned long duration, short waveType) {
    if (!bruceConfig.soundEnabled) return;

    // derived from
    // https://github.com/earlephilhower/ESP8266Audio/blob/master/examples/PlayWAVFromFunction/PlayWAVFromFunction.ino

    if (frequency == 0 || duration == 0) return;

    float hz = frequency;

    AudioGeneratorWAV *wav;
    AudioFileSourceFunction *file;
    AudioOutputI2S *out = new AudioOutputI2S();
    if (!out || !configureI2SPinout(out)) {
        delete out;
        return;
    }

    file = new AudioFileSourceFunction(duration / 1000.0); // , 1, 44100
    //
    // you can set (sec, channels, hz, bit/sample) but you should care about
    // the trade-off between performance and the audio quality
    //
    // file = new AudioFileSourceFunction(sec, channels, hz, bit/sample);
    // channels   : default = 1
    // hz         : default = 8000 (8000, 11025, 22050, 44100, 48000, etc.)
    // bit/sample : default = 16 (8, 16, 32)

    // ===== set your sound function =====

    if (waveType == 0) { // square
        file->addAudioGenerators([&](const float time) {
            float v = (sin(hz * time) >= 0) ? 1.0f : -1.0f;
            ;         // generate square wave
            v *= 0.1; // scale
            return v;
        });
    } else if (waveType == 1) { // sine
        file->addAudioGenerators([&](const float time) {
            float v = sin(TWO_PI * hz * time); // generate sine wave
            v *= fmod(time, 1.f);              // change linear
            v *= 0.1;                          // scale
            return v;
        });
    }
    // TODO: more wave types: triangle, sawtooth
    //
    // sound function should have one argument(float) and one return(float)
    // param  : float (current time [sec] of the song)
    // return : float (the amplitude of sound which varies from -1.f to +1.f)

    wav = new AudioGeneratorWAV();
    wav->begin(file, out);

    while (wav->isRunning()) {
        if (!wav->loop() || check(AnyKeyPress)) wav->stop();
    }

    delete file;
    delete wav;
    delete out;
}

#endif

void _tone(unsigned int frequency, unsigned long duration) {
    if (!bruceConfig.soundEnabled) return;

#if defined(BUZZ_PIN)
    tone(BUZZ_PIN, frequency, duration);
#elif defined(HAS_NS4168_SPKR)
    //  alt. implementation using the speaker
    playTone(frequency, duration, 0);
#endif
}
