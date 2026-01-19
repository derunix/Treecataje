#ifndef __AUDIO_MENU_H__
#define __AUDIO_MENU_H__

#include <MenuItemInterface.h>

class AudioMenu : public MenuItemInterface {
public:
    AudioMenu() : MenuItemInterface("Audio") {}

    void optionsMenu(void);
    void drawIcon(float scale);
    void drawIconImg();
    bool getTheme() { return bruceConfig.theme.audio; }
};

#endif
