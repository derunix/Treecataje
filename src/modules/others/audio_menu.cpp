#include "audio_menu.h"
#include <globals.h>
#include "audio.h"
#include "core/display.h"
#include "core/sd_functions.h"
#include <algorithm>
#include <functional>

#if !defined(HAS_NS4168_SPKR)

void audioRecordingsMenu() { displayWarning("Speaker not available", true); }
void audioPlayerMenu() { displayWarning("Speaker not available", true); }

#else

struct AudioTrack {
    FS *fs = nullptr;
    String path;
    String label;
    String source;
};

static constexpr size_t MAX_AUDIO_TRACKS = 150;
static constexpr int DEFAULT_SCAN_DEPTH = 3;
static constexpr uint32_t RECORDING_BYTE_RATE = 16000 * 2; // mono, 16-bit
static constexpr uint32_t MP3_EST_BITRATE = 128000 / 8;     // 128 kbps -> bytes/sec

enum PlayResult { PLAY_COMPLETED, PLAY_NEXT, PLAY_PREV, PLAY_EXIT, PLAY_FAIL, PLAY_PAUSE };
struct PlayOutcome {
    PlayResult res = PLAY_COMPLETED;
    size_t resumePos = 0;
};

static bool ensureSdMounted() { return sdcardMounted || setupSdCard(); }

static void ensureRecordingFolder(FS *fs, const char *path) {
    if (!fs || !path) return;
    if (!fs->exists(path)) fs->mkdir(path);
}

static String getFilename(const String &path) {
    int idx = path.lastIndexOf('/');
    if (idx < 0 || idx + 1 >= path.length()) return path;
    return path.substring(idx + 1);
}

static void scanFolder(
    FS *fs, const String &source, const String &root, std::vector<AudioTrack> &tracks, int depth, bool recordingsOnly
) {
    if (tracks.size() >= MAX_AUDIO_TRACKS || depth < 0) return;
    if (!fs->exists(root)) return;

    File dir = fs->open(root);
    if (!dir || !dir.isDirectory()) {
        dir.close();
        return;
    }

    File entry;
    while ((entry = dir.openNextFile())) {
        String fullPath = String(entry.path());
        if (entry.isDirectory()) {
            if (!recordingsOnly) scanFolder(fs, source, fullPath, tracks, depth - 1, recordingsOnly);
        } else {
            String lower = fullPath;
            lower.toLowerCase();
            if (isAudioFile(lower) && tracks.size() < MAX_AUDIO_TRACKS) {
                AudioTrack t;
                t.fs = fs;
                t.path = fullPath;
                t.label = getFilename(fullPath);
                t.source = source;
                tracks.push_back(t);
            }
        }
        entry.close();
        if (tracks.size() >= MAX_AUDIO_TRACKS) break;
    }
    dir.close();
}

static std::vector<AudioTrack> collectRecordings() {
    std::vector<AudioTrack> tracks;
    if (littleFsMounted) {
        ensureRecordingFolder(&LittleFS, "/BruceMIC");
        scanFolder(&LittleFS, "Flash", "/BruceMIC", tracks, 0, true);
    }
    if (ensureSdMounted()) {
        ensureRecordingFolder(&SD, "/BruceMIC");
        scanFolder(&SD, "SD", "/BruceMIC", tracks, 0, true);
    }
    std::sort(tracks.begin(), tracks.end(), [](const AudioTrack &a, const AudioTrack &b) { return a.label < b.label; });
    return tracks;
}

static std::vector<AudioTrack> collectAudioLibrary() {
    std::vector<AudioTrack> tracks;
    if (littleFsMounted) scanFolder(&LittleFS, "Flash", "/", tracks, DEFAULT_SCAN_DEPTH, false);
    if (ensureSdMounted()) scanFolder(&SD, "SD", "/", tracks, DEFAULT_SCAN_DEPTH, false);
    std::sort(tracks.begin(), tracks.end(), [](const AudioTrack &a, const AudioTrack &b) { return a.label < b.label; });
    return tracks;
}

static bool deleteTrack(std::vector<AudioTrack> &tracks, int idx) {
    if (idx < 0 || idx >= (int)tracks.size()) return false;
    AudioTrack t = tracks[idx];
    if (t.fs && t.fs->exists(t.path)) {
        if (!t.fs->remove(t.path)) return false;
    }
    tracks.erase(tracks.begin() + idx);
    return true;
}

static uint32_t getWavByteRate(AudioTrack &track) {
    if (!track.fs) return 0;
    File f = track.fs->open(track.path, FILE_READ);
    if (!f) return 0;
    uint8_t hdr[32] = {0};
    int read = f.read(hdr, sizeof(hdr));
    f.close();
    if (read < 32) return 0;
    return hdr[28] | (hdr[29] << 8) | (hdr[30] << 16) | (hdr[31] << 24);
}

static uint32_t getTrackDurationSec(AudioTrack &track) {
    if (!track.fs) return 0;
    File f = track.fs->open(track.path);
    if (!f) return 0;
    size_t sz = f.size();
    f.close();
    uint32_t byteRate = 0;
    if (track.path.endsWith(".wav") && sz > 44) {
        byteRate = getWavByteRate(track);
        if (byteRate == 0) byteRate = RECORDING_BYTE_RATE;
        return (sz - 44) / byteRate;
    }
    if (track.path.endsWith(".mp3") && sz > 1000) { return sz / MP3_EST_BITRATE; }
    return 0;
}

static String formatTime(uint32_t seconds) {
    uint32_t m = seconds / 60;
    uint32_t s = seconds % 60;
    char buf[12];
    snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned int)m, (unsigned int)s);
    return String(buf);
}

static void drawNowPlaying(const AudioTrack &track, int index, int total, const char *title) {
    drawMainBorderWithTitle(title, true);
    printSubtitle("Now Playing", true);
    padprintln(String("#") + (index + 1) + "/" + total + " " + track.label);
    padprintln(String("Source: ") + track.source);
    padprintln("Controls:");
    padprintln(" Prev/Next - track");
    padprintln(" Sel - pause/resume");
    padprintln(" Up/Down - volume");
    padprintln(" Esc - back");
    displayRedStripe("Playing...", TFT_WHITE, bruceConfig.priColor);
}

static PlayOutcome playTrackWithControls(AudioTrack &track, int index, int total, const char *title, size_t startPos = 0) {
    drawNowPlaying(track, index, total, title);

    int volumePercent = bruceConfig.soundVolume;
    float volumeGain = ((float)volumePercent) / 100.0f;

    uint32_t byteRate = track.path.endsWith(".wav") ? getWavByteRate(track) : 0;
    if (byteRate == 0 && track.path.endsWith(".wav")) byteRate = RECORDING_BYTE_RATE;
    if (byteRate == 0 && track.path.endsWith(".mp3")) byteRate = MP3_EST_BITRATE;

    PlayOutcome outcome;
    unsigned long lastUi = 0;
    unsigned long lastInputMs = 0;
    unsigned long cooldownUntil = 0;
    int noisyHits = 0;

    auto debouncedPressed = [&](volatile bool &btn, uint32_t minGapMs = 300) -> bool {
        if (!check(btn)) return false;
        unsigned long now = millis();
        if (now < cooldownUntil || (now - lastInputMs) < minGapMs) {
            // If we keep seeing inputs inside the cooldown window, extend suppression
            noisyHits++;
            if (noisyHits > 4) {
                cooldownUntil = now + 1200;
                noisyHits = 0;
            }
            return false;
        }
        noisyHits = 0;
        lastInputMs = now;
        cooldownUntil = now + 400; // short cooldown to avoid rapid toggling
        return true;
    };

    auto progressCb = [&](size_t pos, size_t size, AudioOutputI2S *out) -> bool {
        if (debouncedPressed(EscPress)) {
            outcome.res = PLAY_EXIT;
            return false;
        }
        if (debouncedPressed(PrevPress)) {
            outcome.res = PLAY_PREV;
            return false;
        }
        if (debouncedPressed(NextPress)) {
            outcome.res = PLAY_NEXT;
            return false;
        }
        if (debouncedPressed(SelPress)) {
            outcome.res = PLAY_PAUSE;
            outcome.resumePos = pos;
            return false;
        }
        if (debouncedPressed(UpPress)) {
            volumePercent = std::min(100, volumePercent + 5);
            volumeGain = ((float)volumePercent) / 100.0f;
            bruceConfig.soundVolume = volumePercent;
            out->SetGain(volumeGain);
        }
        if (debouncedPressed(DownPress)) {
            volumePercent = std::max(0, volumePercent - 5);
            volumeGain = ((float)volumePercent) / 100.0f;
            bruceConfig.soundVolume = volumePercent;
            out->SetGain(volumeGain);
        }

        unsigned long now = millis();
        if (now - lastUi > 200) {
            uint32_t totalSec = (byteRate > 0 && size > 0) ? size / byteRate : 0;
            uint32_t posSec = (byteRate > 0 && pos > 0) ? pos / byteRate : 0;
            String line = "Vol:";
            line += volumePercent;
            line += "% ";
            line += formatTime(posSec);
            if (totalSec > 0) {
                line += "/";
                line += formatTime(totalSec);
            }
            int percent = (size > 0) ? (100 * pos) / size : 0;
            line += " ";
            line += percent;
            line += "%";
            displayRedStripe(line, TFT_WHITE, bruceConfig.priColor);
            lastUi = now;
        }
        return true;
    };

    bool stoppedByCb = false;
    bool played = playAudioFile(track.fs, track.path, progressCb, startPos, &stoppedByCb);
    if (!played) {
        displayError("Playback failed", true);
        outcome.res = PLAY_FAIL;
        return outcome;
    }
    if (outcome.res == PLAY_PAUSE && stoppedByCb) {
        return playTrackWithControls(track, index, total, title, outcome.resumePos);
    }
    return outcome;
}

static PlayOutcome playTrackQueue(std::vector<AudioTrack> &tracks, int startIndex, const char *title) {
    if (tracks.empty() || startIndex < 0 || startIndex >= (int)tracks.size()) return {};

    int idx = startIndex;
    PlayOutcome outcome;

    while (idx >= 0 && idx < (int)tracks.size()) {
        outcome = playTrackWithControls(tracks[idx], idx, tracks.size(), title);

        if (outcome.res == PLAY_COMPLETED) {
            if ((idx + 1) < (int)tracks.size()) {
                idx++;
                continue;
            }
            break;
        }
        if (outcome.res == PLAY_NEXT) {
            idx = (idx + 1) % tracks.size();
            continue;
        }
        if (outcome.res == PLAY_PREV) {
            idx = (idx - 1 + tracks.size()) % tracks.size();
            continue;
        }
        break; // exit, pause handled internally, or playback completed/failed
    }
    return outcome;
}

static void renderTrackMenu(
    const char *title, std::vector<AudioTrack> &tracks, const std::function<void()> &refreshFn, bool showRescan
) {
    bool exit = false;
    int index = 0;
    while (!exit) {
        options.clear();
        int visibleCounter = 1;
        for (int i = 0; i < (int)tracks.size(); i++) {
            uint32_t durationSec = getTrackDurationSec(tracks[i]);
            String itemLabel = String(visibleCounter) + ". " + tracks[i].source + " - " + tracks[i].label;
            if (durationSec > 0) itemLabel += " (" + formatTime(durationSec) + ")";

            options.push_back({itemLabel, [&, i, itemLabel, durationSec]() {
                                   std::vector<Option> sub;
                                   sub.push_back({"Play", [&, i]() {
                                                     playTrackQueue(tracks, i, title);
                                                     drawMainBorderWithTitle(title, true);
                                                 }});
                                   sub.push_back({"Info", [&, i, durationSec]() {
                                                     uint32_t size = 0;
                                                     time_t ts = 0;
                                                     if (tracks[i].fs) {
                                                         File info = tracks[i].fs->open(tracks[i].path, FILE_READ);
                                                         if (info) {
                                                             size = info.size();
                                                             ts = info.getLastWrite();
                                                             info.close();
                                                         }
                                                     }
                                                     drawMainBorderWithTitle("Track Info");
                                                     padprintln("Path:");
                                                     padprintln(tracks[i].path);
                                                     padprintln("");
                                                     padprintln(String("Size: ") + String(size) + " bytes");
                                                     if (durationSec > 0) padprintln(String("Duration: ") + formatTime(durationSec));
                                                     if (ts > 0) {
                                                         struct tm *tmstruct = localtime(&ts);
                                                         char buf[24];
                                                         strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M", tmstruct);
                                                         padprintln(String("Modified: ") + buf);
                                                     }
                                                     while (!check(EscPress) && !check(SelPress)) { vTaskDelay(50 / portTICK_PERIOD_MS); }
                                                 }});
                                   sub.push_back({"Delete", [&, i]() {
                                                     if (deleteTrack(tracks, i)) {
                                                         displaySuccess("Deleted", true);
                                                         refreshFn();
                                                     } else displayError("Delete failed", true);
                                                 }});
                                   sub.push_back({"Back", []() {}});
                                   loopOptions(sub, MENU_TYPE_SUBMENU, itemLabel.c_str());
                               }});
            visibleCounter++;
        }
        if (showRescan) {
            options.push_back({"Rescan library", [&]() {
                                   refreshFn();
                                   index = 0;
                               }});
        }
        options.push_back({"Back", [&]() { exit = true; }});

        index = loopOptions(options, MENU_TYPE_SUBMENU, title, index);
        if (index < 0 || exit || index >= (int)options.size()) exit = true;
    }
}

void audioRecordingsMenu() {
    if (!bruceConfig.soundEnabled) {
        displayWarning("Sound is disabled", true);
        return;
    }

    auto tracks = collectRecordings();
    if (tracks.empty()) {
        displayInfo("No recordings found", true);
        return;
    }

    renderTrackMenu("Recordings", tracks, [&]() { tracks = collectRecordings(); }, true);
}

void audioPlayerMenu() {
    if (!bruceConfig.soundEnabled) {
        displayWarning("Sound is disabled", true);
        return;
    }

    auto tracks = collectAudioLibrary();
    if (tracks.empty()) {
        displayInfo("No audio files found", true);
        return;
    }

    renderTrackMenu("Audio Files", tracks, [&]() { tracks = collectAudioLibrary(); }, true);
}

#endif // HAS_NS4168_SPKR
