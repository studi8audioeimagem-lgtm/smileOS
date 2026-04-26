#ifndef ICONS_H
#define ICONS_H

#include "types.h"

typedef enum {
    ICON_NOTEPAD = 0,
    ICON_TERMINAL,
    ICON_SETTINGS,
    ICON_FILES,
    ICON_CALCULATOR,
    ICON_CLOCK,
    ICON_PALETTE,
    ICON_TYPING,
    ICON_HELP,
    ICON_ABOUT,
    ICON_PHOTOS,
    ICON_MUSIC,
    ICON_BROWSER,
    ICON_WIFI,
    ICON_VOLUME,
    ICON_BATTERY,
    ICON_BELL,
    ICON_SEARCH,
    ICON_TRASH
} icon_id_t;

/* Draw an app icon at (x,y) with square size (pixels). */
void icon_draw(icon_id_t id, i32 x, i32 y, i32 size, bool active);

#endif
