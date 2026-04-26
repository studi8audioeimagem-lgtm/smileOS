#include "kernel.h"

static os_settings_t g_settings;

void settings_init(void) {
    /* Defaults aimed at beginners. Persistence can be added later via VFS. */
    g_settings.tutorial_enabled = true;
    g_settings.tutorial_completed = false;
    g_settings.tutorial_step = 0;
    g_settings.large_icons = true;
    g_settings.dark_theme = true;
    g_settings.large_text = false;
    g_settings.theme_mode = 1; /* Dark */
    g_settings.accent_color = 0x3A7BFF;
    g_settings.dock_autohide = false;
    g_settings.wallpaper_mode = 0;
    g_settings.dnd = false;

    log_write("settings: defaults applied");
}

os_settings_t *settings_get(void) {
    return &g_settings;
}

u32 settings_accent_color(void) {
    return g_settings.accent_color ? g_settings.accent_color : 0x3A7BFF;
}
