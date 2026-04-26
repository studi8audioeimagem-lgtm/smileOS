#include "kernel.h"
#include "../gfx/colors.h"

static bool g_open;
static u64 g_open_tick;

static void draw_background(void) {
    const u32 w = framebuffer_width();
    const u32 h = framebuffer_height();
    if (w == 0 || h == 0) {
        return;
    }

    const u32 top = 0x091120;
    const u32 mid = 0x101B31;
    const u32 bot = 0x060B14;
    for (u32 y = 0; y < h; y++) {
        const u32 t = (y * 255U) / (h ? h : 1);
        const u32 c = (t < 150)
            ? ((top & 0xFEFEFE) + (((mid & 0xFEFEFE) - (top & 0xFEFEFE)) * t) / 150U)
            : ((mid & 0xFEFEFE) + (((bot & 0xFEFEFE) - (mid & 0xFEFEFE)) * (t - 150U)) / 105U);
        framebuffer_fill_rect_fast(0, y, w, 1, c);
    }
}

void startscreen_init(void) {
    g_open = true;
    g_open_tick = scheduler_ticks();
    log_write("desktop: start screen initialized");
    startscreen_draw();
}

bool startscreen_is_open(void) {
    return g_open;
}

bool startscreen_needs_redraw(void) {
    if (!g_open) {
        return false;
    }
    return scheduler_ticks() - g_open_tick < 12ULL;
}

void startscreen_draw(void) {
    if (!framebuffer_ready() || !g_open) {
        return;
    }

    draw_background();

    const u32 sw = framebuffer_width();
    const u32 sh = framebuffer_height();
    const u32 box_w = (sw > 640) ? 520 : (sw > 340 ? sw - 80 : sw - 20);
    const u32 box_h = 240;
    const u32 box_x = (sw - box_w) / 2;
    const u32 box_y = (sh - box_h) / 2;

    draw_shadow_round_rect(box_x, box_y, box_w, box_h, 22);
    draw_round_rect_alpha(box_x, box_y, box_w, box_h, 22, COLOR_PANEL, 220);
    draw_round_rect(box_x, box_y, box_w, box_h, 22, COLOR_PANEL_BORDER);
    draw_round_rect_alpha(box_x + 1, box_y + 1, box_w - 2, 62, 21, settings_accent_color(), 85);

    draw_round_rect_alpha(box_x + 24, box_y + 24, 54, 54, 18, settings_accent_color(), 220);
    text_draw(box_x + 40, box_y + 41, "S", COLOR_WHITE);

    text_draw(box_x + 96, box_y + 26, "SMILEOS", COLOR_WHITE);
    text_draw(box_x + 96, box_y + 50, "A clean desktop built for fast startup.", COLOR_WHITE);
    text_draw(box_x + 24, box_y + 102, "CLICK START OR PRESS ENTER TO CONTINUE", COLOR_WHITE);

    draw_round_rect(box_x + 24, box_y + 150, 160, 36, 14, settings_accent_color());
    text_draw(box_x + 72, box_y + 161, "START", COLOR_WHITE);

    draw_round_rect_alpha(box_x + 200, box_y + 150, 224, 36, 14, COLOR_TILE, 170);
    text_draw(box_x + 238, box_y + 161, "TUTORIAL ENABLED", COLOR_WHITE);

    framebuffer_damage_full();
}

static bool inside_start_button(i32 x, i32 y) {
    const u32 sw = framebuffer_width();
    const u32 sh = framebuffer_height();
    const u32 box_w = (sw > 640) ? 520 : (sw > 340 ? sw - 80 : sw - 20);
    const u32 box_h = 240;
    const i32 box_x = (i32)(sw - box_w) / 2;
    const i32 box_y = (i32)(sh - box_h) / 2;
    return x >= box_x + 24 && x < box_x + 184 && y >= box_y + 150 && y < box_y + 186;
}

static void dismiss(void) {
    if (!g_open) {
        return;
    }
    g_open = false;
    desktop_redraw();
    taskbar_init();
    framebuffer_damage_full();
}

bool startscreen_handle_input(const input_event_t *evt, i32 cursor_x, i32 cursor_y) {
    if (!evt || !g_open) {
        return false;
    }

    if (evt->type == INPUT_EVENT_MOUSE_BUTTON) {
        if (evt->a) {
            /* Only Start button dismisses. This avoids "first click eaten" confusion. */
            if (inside_start_button(cursor_x, cursor_y)) {
                dismiss();
            }
            return true;
        }
        return true;
    }

    if (evt->type == INPUT_EVENT_KEY && evt->a) {
        if (evt->a == '\n' || evt->a == KEY_ESC || evt->a == ' ') {
            dismiss();
            return true;
        }
        return true;
    }

    return true;
}
