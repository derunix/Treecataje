#ifndef __POWER_MENU_H__
#define __POWER_MENU_H__

#include <MenuItemInterface.h>

class PowerMenu : public MenuItemInterface {
public:
    PowerMenu() : MenuItemInterface("Power") {}

    void optionsMenu(void);
    void drawIcon(float scale);
    void drawIconImg();
    bool getTheme() { return bruceConfig.theme.others; } // Use Others theme for now
};

#endif
