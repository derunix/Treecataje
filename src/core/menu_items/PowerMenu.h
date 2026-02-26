#ifndef __POWER_MENU_H__
#define __POWER_MENU_H__

#include <MenuItemInterface.h>

class PowerMenu : public MenuItemInterface {
public:
    PowerMenu() : MenuItemInterface("Power") {}

    void optionsMenu(void);
    void drawIcon(float scale);
    bool hasTheme() { return bruceConfig.theme.power; }
    String themePath() { return bruceConfig.theme.paths.power; }
};

#endif
