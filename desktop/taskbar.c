#include "kernel.h"
#include "../gfx/colors.h"
#include "icons.h"

typedef struct {
    icon_id_t icon;
    void (*open_fn)(void);
    const char *title;
} dock_item_t;

/* Keep the dock list stable; desktop click hit tests call dock_handle_input(). */
static const dock_item_t g_dock[] = {
    {ICON_NOTEPAD, apps_open_notepad, "NOTEPAD"},
    {ICON_TERMINAL, apps_open_bash_terminal, "TERMINAL"},
    {ICON_CALCULATOR, apps_open_calculator, "CALC"},
    {ICON_FILES, apps_open_files, "FILES"},
    {ICON_BROWSER, apps_open_browser, "WEB"},
    {ICON_CLOCK, apps_open_clock, "CLOCK"},
    {ICON_PALETTE, apps_open_palette, "PALETTE"},
    {ICON_TYPING, apps_open_typing, "TYPING"},
    {ICON_HELP, apps_open_help, "HELP"},
    {ICON_ABOUT, apps_open_about, "ABOUT"},
    {ICON_TRASH, apps_open_trash, "TRASH"},
};

static bool g_left_down;
static u16 g_scales_cur[16];
static bool g_scales_init;
static bool g_dock_animating;

static bool streq(const char *a, const char *b) {
    if (!a || !b) return false;
    usize i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return false;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

static void dock_rect(i32 *x, i32 *y, i32 *w, i32 *h) {
    const i32 sw = (i32)framebuffer_width();
    const i32 sh = (i32)framebuffer_height();
    const i32 item = 40;
    const i32 gap = 10;
    const i32 count = (i32)(sizeof(g_dock) / sizeof(g_dock[0]));
    const i32 dock_w = count * item + (count - 1) * gap + 24;
    const i32 dock_h = 58;
    const i32 dx = (sw > dock_w) ? (sw - dock_w) / 2 : 0;
    const i32 dy = sh - dock_h - 14;
    if (x) *x = dx;
    if (y) *y = dy;
    if (w) *w = dock_w;
    if (h) *h = dock_h;
}

static void menubar_draw(void) {
    if (!framebuffer_ready()) return;
    const u32 w = framebuffer_width();
    const u32 h = 28;

    /* Glassy top bar */
    draw_blur_rect(0, 0, w, h, 3);
    draw_rect_alpha(0, 0, w, h, COLOR_PANEL, 210);
    draw_rect(0, h - 1, w, 1, COLOR_PANEL_BORDER);

    /* Logo + menus (minimal). */
    draw_round_rect_alpha(10, 6, 16, 16, 6, settings_accent_color(), 220);
    text_draw(14, 10, "S", COLOR_WHITE);

    const char *app = apps_has_active_window() ? apps_running_title() : "DESKTOP";
    if (app) {
        text_draw(36, 10, app, COLOR_WHITE);
    }

    /* Right side: fake status + clock from uptime mm:ss. */
    const u64 ticks = scheduler_ticks();
    const u32 secs = (u32)(ticks / 50ULL);
    const u32 mm = (secs / 60U) % 100U;
    const u32 ss = secs % 60U;
    char buf[6];
    buf[0] = (char)('0' + (mm / 10U));
    buf[1] = (char)('0' + (mm % 10U));
    buf[2] = ':';
    buf[3] = (char)('0' + (ss / 10U));
    buf[4] = (char)('0' + (ss % 10U));
    buf[5] = 0;

    /* Control center button */
    {
        const u32 bx = (w > 128) ? (w - 118) : 0;
        draw_round_rect_alpha(bx, 6, 34, 16, 8, COLOR_TILE, 120);
        text_draw(bx + 12, 10, "[]", COLOR_WHITE);
    }

    text_draw((w > 74) ? (w - 70) : 0, 10, buf, COLOR_WHITE);
}

static void draw_indicator_dot(i32 cx, i32 y) {
    /* tiny circle */
    const i32 r = 2;
    for (i32 dy = -r; dy <= r; dy++) {
        for (i32 dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r * r) continue;
            const i32 px = cx + dx;
            const i32 py = y + dy;
            if (px < 0 || py < 0) continue;
            if ((u32)px >= framebuffer_width() || (u32)py >= framebuffer_height()) continue;
            framebuffer_put_pixel((u32)px, (u32)py, COLOR_WHITE);
        }
    }
}

static void dock_draw(void) {
    if (!framebuffer_ready()) return;

    i32 mx = 0, my = 0;
    ui_cursor_get_pos(&mx, &my);

    os_settings_t *s = settings_get();
    if (s->dock_autohide && !startmenu_is_open()) {
        const i32 sh = (i32)framebuffer_height();
        if (my < sh - 90) {
            return;
        }
    }

    i32 x, y, w, h;
    dock_rect(&x, &y, &w, &h);

    /* Glassy dock */
    if (w > 0 && h > 0) {
        draw_blur_rect((u32)x, (u32)y, (u32)w, (u32)h, 3);
    }
    const u32 r = 16;
    draw_shadow_round_rect((u32)x, (u32)y, (u32)w, (u32)h, r);
    draw_round_rect_alpha((u32)x, (u32)y, (u32)w, (u32)h, r, COLOR_PANEL, 185);
    draw_round_rect_alpha((u32)x + 1, (u32)y + 1, (u32)w - 2, (u32)h - 2, r - 1, COLOR_TILE, 55);

    const i32 item_base = 40;
    const i32 gap = 10;
    const i32 count = (i32)(sizeof(g_dock) / sizeof(g_dock[0]));
    const i32 inner_x = x + 12;
    const i32 inner_y = y + 9;

    /* magnify: compute per-item scale based on mouse distance (fixed-point 8.8). */
    u16 scales[16];
    if (count > 16) return;

    const u16 one = 256;
    const u16 max_scale = 397; /* ~1.55 */
    const i32 radius = 90;

    for (i32 i = 0; i < count; i++) {
        const i32 cx = inner_x + i * (item_base + gap) + item_base / 2;
        i32 ad = mx - cx;
        if (ad < 0) ad = -ad;
        u16 t = 0;
        if (ad < radius) {
            /* t in 0..256 */
            t = (u16)(((radius - ad) * 256) / radius);
        }
        const u16 s = (u16)(one + (u16)(((u32)(max_scale - one) * (u32)t) / 256U));
        scales[i] = s;
    }

    if (!g_scales_init) {
        for (i32 i = 0; i < count; i++) {
            g_scales_cur[i] = scales[i];
        }
        g_scales_init = true;
    }

    /* Ease current scales towards target. */
    g_dock_animating = false;
    for (i32 i = 0; i < count; i++) {
        const i32 cur = (i32)g_scales_cur[i];
        const i32 tgt = (i32)scales[i];
        const i32 delta = tgt - cur;
        if (delta != 0) {
            i32 step = delta / 3;
            if (step == 0) step = (delta > 0) ? 1 : -1;
            g_scales_cur[i] = (u16)(cur + step);
            g_dock_animating = true;
        }
    }

    /* Layout with variable scales, centered within dock (fixed-point). */
    i32 content_w = 0;
    for (i32 i = 0; i < count; i++) {
        content_w += (i32)(((u32)g_scales_cur[i] * (u32)item_base) / 256U);
        if (i + 1 < count) content_w += gap;
    }
    i32 start_x = inner_x + ((w - 24) - content_w) / 2;
    if (start_x < inner_x) start_x = inner_x;

    const char *running = apps_has_running_app() ? apps_running_title() : 0;

    i32 fx = start_x;
    for (i32 i = 0; i < count; i++) {
        const i32 sz = (i32)(((u32)g_scales_cur[i] * (u32)item_base) / 256U);
        const i32 ix = fx;
        const i32 iy = inner_y + (item_base - sz) / 2;

        const bool active = (running && g_dock[i].title && streq(running, g_dock[i].title));
        icon_draw(g_dock[i].icon, ix, iy, sz, active);

        /* Running indicator */
        if (running && g_dock[i].open_fn && g_dock[i].title && streq(running, g_dock[i].title)) {
            draw_indicator_dot(ix + sz / 2, y + h - 10);
        }

        fx += sz + gap;
    }
}

void taskbar_init(void) {
    /* Redraw-only; do not log (this can be called every mouse move). */
    if (!framebuffer_ready()) {
        return;
    }
    const u32 sw = framebuffer_width();
    const u32 sh = framebuffer_height();

    menubar_draw();
    framebuffer_damage_rect(0, 0, (i32)sw, 28);

    startmenu_draw();
    if (startmenu_needs_redraw()) {
        /* Approximate rect (start menu lives bottom-left). */
        const i32 mw = (sw > 520) ? 380 : (i32)sw - 20;
        const i32 mh = (sh > 560) ? 420 : (i32)sh - 90;
        const i32 mx = 10;
        const i32 my = (sh > (u32)(mh + 52)) ? (i32)(sh - (u32)mh - 52) : 0;
        framebuffer_damage_rect(mx - 18, my - 18, mw + 36, mh + 54);
    }

    /* Dock (support auto-hide by repainting wallpaper behind it). */
    {
        i32 dx, dy, dw, dh;
        dock_rect(&dx, &dy, &dw, &dh);
        i32 mx = 0, my = 0;
        ui_cursor_get_pos(&mx, &my);
        os_settings_t *s = settings_get();
        const bool autohide_hide = s->dock_autohide && !startmenu_is_open() && (my < (i32)sh - 90);
        if (autohide_hide) {
            /* Erase the dock area by redrawing wallpaper only (desktop icons rarely live here). */
            const i32 ex = dx - 26;
            const i32 ey = dy - 20;
            const i32 ew = dw + 52;
            const i32 eh = dh + 40;
            if (ex < (i32)sw && ey < (i32)sh && ex + ew > 0 && ey + eh > 0) {
                const u32 cx = (ex < 0) ? 0U : (u32)ex;
                const u32 cy = (ey < 0) ? 0U : (u32)ey;
                u32 cw = (u32)ew;
                u32 ch = (u32)eh;
                if (cx + cw > sw) cw = sw - cx;
                if (cy + ch > sh) ch = sh - cy;
                desktop_draw_wallpaper_rect(cx, cy, cw, ch);
                framebuffer_damage_rect(ex, ey, ew, eh);
            }
        } else {
            dock_draw();
            framebuffer_damage_rect(dx - 26, dy - 20, dw + 52, dh + 40);
        }
    }

    notifications_draw();
    if (notifications_needs_redraw()) {
        /* Toast bottom-right and center panel right side. */
        framebuffer_damage_rect((i32)sw - 360, (i32)sh - 140, 360, 140);
        framebuffer_damage_rect((i32)sw - 360, 28, 360, (i32)sh - 28);
    }
}

bool dock_needs_redraw(void) {
    /* If easing is mid-flight, we need per-tick redraw. */
    return g_dock_animating;
}

bool dock_handle_input(const input_event_t *evt, i32 cursor_x, i32 cursor_y) {
    if (!evt || !framebuffer_ready()) {
        return false;
    }

    /* Start menu click lives in the menu bar logo area. */
    if (evt->type == INPUT_EVENT_MOUSE_BUTTON) {
        const bool left = evt->a ? true : false;
        const bool rising = left && !g_left_down;
        g_left_down = left;
        if (!rising) {
            return false;
        }

        /* Menu bar logo */
        if (cursor_y >= 0 && cursor_y < 28 && cursor_x >= 8 && cursor_x < 34) {
            startmenu_toggle();
            desktop_redraw();
            taskbar_init();
            return true;
        }

        /* Control center */
        {
            const u32 w = framebuffer_width();
            const i32 bx = (w > 128) ? (i32)(w - 118) : 0;
            if (cursor_y >= 6 && cursor_y < 22 && cursor_x >= bx && cursor_x < bx + 34) {
                notifications_toggle_center();
                desktop_redraw();
                taskbar_init();
                return true;
            }
        }

        if (startmenu_handle_input(evt, cursor_x, cursor_y)) {
            desktop_redraw();
            taskbar_init();
            return true;
        }

        /* Dock items */
        {
            os_settings_t *s = settings_get();
            if (s->dock_autohide && !startmenu_is_open()) {
                const i32 sh = (i32)framebuffer_height();
                if (cursor_y < sh - 90) {
                    return false;
                }
            }
        }

        i32 dx, dy, dw, dh;
        dock_rect(&dx, &dy, &dw, &dh);
        if (cursor_x < dx || cursor_x >= dx + dw || cursor_y < dy || cursor_y >= dy + dh) {
            return false;
        }

        /* Hit-test against the same magnified layout we draw. */
        const i32 item_base = 40;
        const i32 gap = 10;
        const i32 count = (i32)(sizeof(g_dock) / sizeof(g_dock[0]));
        const i32 inner_x = dx + 12;
        const i32 inner_y = dy + 9;
        if (count > 16) {
            return true;
        }

        /* Ensure scales are initialized even if you click without hovering first. */
        if (!g_scales_init) {
            for (i32 i = 0; i < count; i++) {
                g_scales_cur[i] = 256;
            }
            g_scales_init = true;
        }

        i32 content_w = 0;
        for (i32 i = 0; i < count; i++) {
            content_w += (i32)(((u32)g_scales_cur[i] * (u32)item_base) / 256U);
            if (i + 1 < count) content_w += gap;
        }
        i32 fx = inner_x + ((dw - 24) - content_w) / 2;
        if (fx < inner_x) fx = inner_x;

        for (i32 i = 0; i < count; i++) {
            const i32 sz = (i32)(((u32)g_scales_cur[i] * (u32)item_base) / 256U);
            const i32 ix = fx;
            const i32 iy = inner_y + (item_base - sz) / 2;

            if (cursor_x >= ix && cursor_x < ix + sz && cursor_y >= iy && cursor_y < iy + sz) {
                if (g_dock[i].open_fn) g_dock[i].open_fn();
                desktop_redraw();
                taskbar_init();
                return true;
            }
            fx += sz + gap;
        }

        return true;
    }

    return false;
}
