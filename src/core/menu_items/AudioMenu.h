#ifndef __AUDIO_MENU_H__
#define __AUDIO_MENU_H__

#include <MenuItemInterface.h>

class AudioMenu : public MenuItemInterface {
public:
    AudioMenu() : MenuItemInterface("Audio") {}

    void optionsMenu(void);
    void drawIcon(float scale);
    bool hasTheme() { return bruceConfig.theme.audio; }
    String themePath() { return bruceConfig.theme.paths.audio; }
};

#endif
