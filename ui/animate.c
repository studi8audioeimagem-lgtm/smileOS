#include "kernel.h"

void ui_animate_tick(void) {
    /* Run animations even when there are no input events. */
    static u64 last_tick;
    const u64 now = scheduler_ticks();
    if (now == last_tick) {
        return;
    }
    last_tick = now;

    const bool need_apps = apps_needs_redraw();
    const bool need_menu = startmenu_needs_redraw() || notifications_needs_redraw();
    const bool need_dock = dock_needs_redraw();
    const bool need_start = startscreen_is_open() && startscreen_needs_redraw();

    if ((!need_apps && !need_menu && !need_dock && !need_start) || !framebuffer_ready()) {
        return;
    }

    /* Redraw in a stable order with the cursor treated as an overlay. */
    ui_cursor_begin_overlay();
    if (startscreen_is_open()) {
        startscreen_draw();
    } else if (need_apps || need_menu) {
        /* These cases can leave remnants if we don't restore the background. */
        desktop_redraw();
        if (apps_has_running_app()) {
            apps_redraw();
        }
        taskbar_init();
    } else if (need_dock) {
        /* Dock easing redraw: it repaints its own background region. */
        taskbar_init();
    }
    ui_cursor_end_overlay();
}
