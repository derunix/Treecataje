#include "AudioMenu.h"
#include "core/display.h"
#include "core/utils.h"
#include "modules/others/audio_menu.h"
#include "modules/others/mic.h"

void AudioMenu::optionsMenu() {
    options.clear();

#if defined(MIC_SPM1423)
    options.push_back({"Live Spectrum", mic_test});
    options.push_back({"Record Mic",   mic_record});
#endif

#if defined(HAS_NS4168_SPKR)
    options.push_back({"Dictaphone", audioRecordingsMenu});
    options.push_back({"Audio Player", audioPlayerMenu});
#endif

    if (options.empty()) {
        displayError("Audio features unavailable", true);
        return;
    }

    addOptionToMainMenu();
    loopOptions(options, MENU_TYPE_SUBMENU, "Audio");
}

void AudioMenu::drawIcon(float scale) {
    clearIconArea();
    int bodyW = scale * 48;
    int bodyH = scale * 36;
    int x = iconCenterX - bodyW / 2;
    int y = iconCenterY - bodyH / 2;

    // Speaker body
    tft.drawRoundRect(x, y, bodyW, bodyH, bodyH / 6, bruceConfig.priColor);
    tft.fillRoundRect(x + bodyW / 8, y + bodyH / 4, bodyW / 3, bodyH / 2, bodyH / 8, bruceConfig.priColor);

    // Waves
    int xOffset = x + (3 * bodyW) / 4;
    for (int i = 0; i < 3; i++) {
        int radius = bodyH / 3 + i * scale * 6;
        tft.drawArc(
            xOffset,
            iconCenterY,
            radius,
            radius + scale * 3,
            280,
            340,
            bruceConfig.priColor,
            bruceConfig.bgColor
        );
    }
}
