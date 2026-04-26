#include "kernel.h"
#include "../gfx/colors.h"

static void draw_window(u32 x, u32 y, u32 width, u32 height, u32 title_color) {
    if (width < 20 || height < 20) {
        return;
    }

    draw_rect(x, y, width, height, COLOR_PANEL_BORDER);
    draw_rect(x + 2, y + 2, width - 4, height - 4, COLOR_WINDOW_BG);
    draw_rect(x + 2, y + 2, width - 4, 28, title_color);
}

void ui_render_init(void) {
    if (framebuffer_ready()) {
        u32 w = framebuffer_width();
        u32 h = framebuffer_height();
        u32 usable_h = (h > 42) ? (h - 42) : h;

        draw_window(w / 4, usable_h / 6, w / 2, usable_h / 2, COLOR_WINDOW_TITLE);
        draw_window((w * 12) / 25, usable_h / 4, w / 3, usable_h / 3, COLOR_ACCENT);
        draw_rect((w * 12) / 25 + 16, usable_h / 4 + 48, (w / 3) - 32, (usable_h / 3) - 64, COLOR_TILE);
    }
    log_write("ui: compositing renderer initialized");
}
