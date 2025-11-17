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

static bool ensureSdMounted() { return sdcardMounted || setupSdCard(); }

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
    if (littleFsMounted) scanFolder(&LittleFS, "Flash", "/BruceMIC", tracks, 0, true);
    if (ensureSdMounted()) scanFolder(&SD, "SD", "/BruceMIC", tracks, 0, true);
    std::sort(
        tracks.begin(),
        tracks.end(),
        [](const AudioTrack &a, const AudioTrack &b) { return a.label < b.label; }
    );
    return tracks;
}

static std::vector<AudioTrack> collectAudioLibrary() {
    std::vector<AudioTrack> tracks;
    if (littleFsMounted) scanFolder(&LittleFS, "Flash", "/", tracks, DEFAULT_SCAN_DEPTH, false);
    if (ensureSdMounted()) scanFolder(&SD, "SD", "/", tracks, DEFAULT_SCAN_DEPTH, false);
    std::sort(
        tracks.begin(),
        tracks.end(),
        [](const AudioTrack &a, const AudioTrack &b) { return a.label < b.label; }
    );
    return tracks;
}

static void drawNowPlaying(const AudioTrack &track, int index, int total, const char *title) {
    drawMainBorderWithTitle(title);
    printSubtitle("Now Playing", true);
    padprintln(String("Track ") + (index + 1) + " of " + total);
    padprintln(String("Source: ") + track.source);
    padprintln(String("File:   ") + track.label);
    padprintln("");
    padprintln("Controls:");
    padprintln(" Prev/Next - change track");
    padprintln(" Sel       - replay");
    padprintln(" Esc       - exit");
    displayRedStripe("Playing... press Prev/Next/Sel/Esc", TFT_WHITE, bruceConfig.priColor);
}

static void playPlaylist(std::vector<AudioTrack> &tracks, int startIndex, const char *title) {
    if (tracks.empty()) return;
    int index = startIndex % tracks.size();
    bool exit = false;

    while (!exit) {
        drawNowPlaying(tracks[index], index, tracks.size(), title);

        bool played = playAudioFile(tracks[index].fs, tracks[index].path);
        if (!played) {
            displayError("Playback failed", true);
            return; // avoid hammering I2S on repeated failures
        }

        // Wait for explicit user action; no auto-advance
        while (true) {
            vTaskDelay(50 / portTICK_PERIOD_MS);
            if (check(EscPress)) {
                exit = true;
                break;
            }
            if (check(PrevPress)) {
                index = (index == 0) ? tracks.size() - 1 : index - 1;
                break;
            }
            if (check(NextPress)) {
                index = (index + 1) % tracks.size();
                break;
            }
            if (check(SelPress)) break; // replay same track
        }
    }
}

static void renderTrackMenu(
    const char *title, std::vector<AudioTrack> &tracks, const std::function<void()> &refreshFn, bool showRescan
) {
    bool exit = false;
    int index = 0;
    while (!exit) {
        options.clear();
        for (int i = 0; i < (int)tracks.size(); i++) {
            String label = String(i + 1) + ". " + tracks[i].source + " - " + tracks[i].label;
            options.push_back({label, [&, i]() { playPlaylist(tracks, i, title); }});
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
