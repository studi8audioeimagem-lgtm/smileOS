#include "kernel.h"
#include "../gfx/colors.h"

typedef struct {
    const char *label;
    void (*open_fn)(void);
} start_item_t;

static bool g_open;
static u64 g_anim_tick;
static bool g_anim_opening;

static const start_item_t g_items[] = {
    {"NOTEPAD", apps_open_notepad},
    {"BASH TERMINAL", apps_open_bash_terminal},
    {"SYSCONFIG", apps_open_sysconfig},
    {"FILES", apps_open_files},
    {"WEB BROWSER", apps_open_browser},
    {"CALCULATOR", apps_open_calculator},
    {"CLOCK", apps_open_clock},
    {"PALETTE", apps_open_palette},
    {"TYPING PRACTICE", apps_open_typing},
    {"HELP", apps_open_help},
    {"ABOUT", apps_open_about},
};

static void menu_rect(u32 *x, u32 *y, u32 *w, u32 *h) {
    const u32 sw = framebuffer_width();
    const u32 sh = framebuffer_height();
    const u32 mw = (sw > 520) ? 380 : (sw - 20);
    const u32 mh = (sh > 560) ? 420 : (sh - 90);
    const u32 mx = 10;
    const u32 my = (sh > (mh + 52)) ? (sh - mh - 52) : 0;
    if (x) *x = mx;
    if (y) *y = my;
    if (w) *w = mw;
    if (h) *h = mh;
}

void startmenu_init(void) {
    g_open = false;
    /* Initialize "in the past" so a closed menu doesn't draw during early boot ticks. */
    g_anim_tick = ~0ULL;
    g_anim_opening = false;
    log_write("desktop: start menu initialized");
}

bool startmenu_is_open(void) {
    return g_open;
}

void startmenu_toggle(void) {
    g_anim_tick = scheduler_ticks();
    g_anim_opening = !g_open;
    g_open = !g_open;
}

bool startmenu_needs_redraw(void) {
    const u64 dt = scheduler_ticks() - g_anim_tick;
    if (dt < 18) {
        return true;
    }
    return g_open;
}

void startmenu_draw(void) {
    if (!framebuffer_ready()) {
        return;
    }
    const u64 now = scheduler_ticks();
    const u64 dt = now - g_anim_tick;
    u32 prog = (dt > 14) ? 255 : (u32)(dt * 255 / 14);
    if (!g_open) {
        prog = 255 - prog;
    }
    if (prog == 0) {
        return;
    }

    u32 x, y, w, h;
    menu_rect(&x, &y, &w, &h);

    const u32 off = (u32)(((255 - prog) * 18U) / 255U);
    const u32 yy = y + off;

    draw_blur_rect(x, yy, w, h, 3);
    draw_shadow_round_rect(x, yy, w, h, 14);
    draw_round_rect_alpha(x, yy, w, h, 14, COLOR_PANEL, 205);
    draw_round_rect(x, yy, w, h, 14, COLOR_PANEL_BORDER);
    draw_round_rect_alpha(x + 1, yy + 1, w - 2, 44, 13, settings_accent_color(), 70);
    text_draw(x + 14, yy + 16, "START", COLOR_WHITE);

    const u32 item_h = 34;
    u32 iy = yy + 56;
    for (usize i = 0; i < (sizeof(g_items) / sizeof(g_items[0])); i++) {
        if (iy + item_h > yy + h - 10) {
            break;
        }
        draw_round_rect_alpha(x + 10, iy, w - 20, item_h, 10, COLOR_TILE, 160);
        text_draw(x + 18, iy + 10, g_items[i].label, COLOR_WHITE);
        iy += item_h + 8;
    }
}

bool startmenu_handle_input(const input_event_t *evt, i32 cursor_x, i32 cursor_y) {
    if (!evt || !framebuffer_ready()) {
        return false;
    }

    if (!g_open) {
        return false;
    }

    if (evt->type != INPUT_EVENT_MOUSE_BUTTON) {
        return true;
    }

    const bool left = evt->a ? true : false;
    if (!left) {
        return true;
    }

    u32 x, y, w, h;
    menu_rect(&x, &y, &w, &h);

    /* Click outside closes. */
    if (cursor_x < (i32)x || cursor_y < (i32)y ||
        cursor_x >= (i32)(x + w) || cursor_y >= (i32)(y + h)) {
        g_open = false;
        return true;
    }

    /* Item hit test. */
    const i32 item_x0 = (i32)x + 10;
    const i32 item_x1 = (i32)(x + w) - 10;
    const i32 item_h = 34;
    i32 iy = (i32)y + 56;

    for (usize i = 0; i < (sizeof(g_items) / sizeof(g_items[0])); i++) {
        const i32 y0 = iy;
        const i32 y1 = iy + item_h;
        if (cursor_x >= item_x0 && cursor_x < item_x1 && cursor_y >= y0 && cursor_y < y1) {
            g_open = false;
            if (g_items[i].open_fn) {
                notifications_post("OPENING APP...");
                g_items[i].open_fn();
            }
            return true;
        }
        iy += item_h + 8;
        if (iy >= (i32)(y + h - 10)) {
            break;
        }
    }

    return true;
}
