#include "kernel.h"
#include "../gfx/colors.h"
#include "string.h"

#define CUR_W 23
#define CUR_H 23

static i32 g_cur_x;
static i32 g_cur_y;
static bool g_cur_visible;
static u32 g_cur_backing[CUR_W * CUR_H];
static i32 g_cur_last_x;
static i32 g_cur_last_y;
static bool g_mouse_left;
static cursor_state_t g_cursor_state;

static void cursor_save(void);
static void cursor_draw(void);

void ui_cursor_set_state(cursor_state_t state) {
    g_cursor_state = state;
}

cursor_state_t ui_cursor_get_state(void) {
    return g_cursor_state;
}

void ui_cursor_get_pos(i32 *x, i32 *y) {
    if (x) *x = g_cur_x;
    if (y) *y = g_cur_y;
}

static void cursor_clamp(void) {
    if (!framebuffer_ready()) {
        return;
    }
    /* g_cur_x/g_cur_y represent the cursor hotspot (center of the circle). */
    const i32 half_w = CUR_W / 2;
    const i32 half_h = CUR_H / 2;
    const i32 min_x = half_w;
    const i32 min_y = half_h;
    const i32 max_x = (i32)framebuffer_width() - 1 - half_w;
    const i32 max_y = (i32)framebuffer_height() - 1 - half_h;
    if (g_cur_x < min_x) g_cur_x = min_x;
    if (g_cur_y < min_y) g_cur_y = min_y;
    if (g_cur_x > max_x) g_cur_x = max_x;
    if (g_cur_y > max_y) g_cur_y = max_y;
}

static void cursor_restore(void) {
    if (!framebuffer_ready() || !g_cur_visible) {
        return;
    }

    const i32 base_x = g_cur_last_x - (CUR_W / 2);
    const i32 base_y = g_cur_last_y - (CUR_H / 2);

    for (u32 y = 0; y < CUR_H; y++) {
        for (u32 x = 0; x < CUR_W; x++) {
            const u32 px = (u32)(base_x + (i32)x);
            const u32 py = (u32)(base_y + (i32)y);
            if (px >= framebuffer_width() || py >= framebuffer_height()) {
                continue;
            }
            framebuffer_put_pixel(px, py, g_cur_backing[y * CUR_W + x]);
        }
    }
}

void ui_cursor_begin_overlay(void) {
    cursor_restore();
}

void ui_cursor_end_overlay(void) {
    if (!framebuffer_ready()) {
        return;
    }

    /* We need to update both the old cursor area (to erase it) and the new one (to draw it). */
    const i32 old_x = g_cur_last_x;
    const i32 old_y = g_cur_last_y;

    cursor_clamp();

    const i32 new_x = g_cur_x;
    const i32 new_y = g_cur_y;

    cursor_save();
    cursor_draw();
    g_cur_visible = true;

    /* Compute union rect (cursor old/new) */
    i32 x0 = old_x - (CUR_W / 2);
    i32 y0 = old_y - (CUR_H / 2);
    i32 x1 = old_x + (CUR_W / 2) + 1;
    i32 y1 = old_y + (CUR_H / 2) + 1;

    const i32 nx0 = new_x - (CUR_W / 2);
    const i32 ny0 = new_y - (CUR_H / 2);
    const i32 nx1 = new_x + (CUR_W / 2) + 1;
    const i32 ny1 = new_y + (CUR_H / 2) + 1;

    if (nx0 < x0) x0 = nx0;
    if (ny0 < y0) y0 = ny0;
    if (nx1 > x1) x1 = nx1;
    if (ny1 > y1) y1 = ny1;

    /* Include any UI redraw damage since the last present. */
    u32 dx = 0, dy = 0, dw = 0, dh = 0;
    bool full = false;
    const bool have_damage = framebuffer_damage_get(&dx, &dy, &dw, &dh, &full);
    if (have_damage) {
        if (full) {
            framebuffer_present();
            framebuffer_damage_clear();
            g_cur_last_x = new_x;
            g_cur_last_y = new_y;
            return;
        }
        const i32 ddx0 = (i32)dx;
        const i32 ddy0 = (i32)dy;
        const i32 ddx1 = (i32)(dx + dw);
        const i32 ddy1 = (i32)(dy + dh);
        if (ddx0 < x0) x0 = ddx0;
        if (ddy0 < y0) y0 = ddy0;
        if (ddx1 > x1) x1 = ddx1;
        if (ddy1 > y1) y1 = ddy1;
    }

    framebuffer_damage_clear();

    /* Clamp to screen and present the union region. */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    const u32 sw = framebuffer_width();
    const u32 sh = framebuffer_height();
    if ((u32)x0 >= sw || (u32)y0 >= sh) {
        g_cur_last_x = new_x;
        g_cur_last_y = new_y;
        return;
    }
    if ((u32)x1 > sw) x1 = (i32)sw;
    if ((u32)y1 > sh) y1 = (i32)sh;

    framebuffer_present_rect((u32)x0, (u32)y0, (u32)(x1 - x0), (u32)(y1 - y0));

    g_cur_last_x = new_x;
    g_cur_last_y = new_y;
}

static void cursor_save(void) {
    if (!framebuffer_ready()) {
        return;
    }

    const i32 base_x = g_cur_x - (CUR_W / 2);
    const i32 base_y = g_cur_y - (CUR_H / 2);

    for (u32 y = 0; y < CUR_H; y++) {
        for (u32 x = 0; x < CUR_W; x++) {
            const u32 px = (u32)(base_x + (i32)x);
            const u32 py = (u32)(base_y + (i32)y);
            if (px >= framebuffer_width() || py >= framebuffer_height()) {
                g_cur_backing[y * CUR_W + x] = 0;
                continue;
            }
            g_cur_backing[y * CUR_W + x] = framebuffer_get_pixel(px, py);
        }
    }
}

static void cursor_draw(void) {
    if (!framebuffer_ready()) {
        return;
    }

    const i32 base_x = g_cur_x - (CUR_W / 2);
    const i32 base_y = g_cur_y - (CUR_H / 2);
    const i32 cx = CUR_W / 2;
    const i32 cy = CUR_H / 2;
    const i32 r = 9;
    const i32 r2 = r * r;
    const i32 inner = r - 2;
    const i32 inner2 = inner * inner;

    for (u32 y = 0; y < CUR_H; y++) {
        for (u32 x = 0; x < CUR_W; x++) {
            const i32 dx = (i32)x - cx;
            const i32 dy = (i32)y - cy;
            const i32 d2 = dx * dx + dy * dy;
            if (d2 > r2) {
                continue;
            }

            const u32 px = (u32)(base_x + (i32)x);
            const u32 py = (u32)(base_y + (i32)y);
            if (px >= framebuffer_width() || py >= framebuffer_height()) {
                continue;
            }
            const u32 color = (d2 >= inner2) ? COLOR_WHITE : COLOR_BLACK;
            framebuffer_put_pixel(px, py, color);
        }
    }

    /* Cursor state glyphs inside the circle. */
    if (g_cursor_state == CURSOR_TEXT) {
        /* I-beam */
        for (i32 y = cy - 6; y <= cy + 6; y++) {
            const u32 px = (u32)(base_x + cx);
            const u32 py = (u32)(base_y + y);
            if (px < framebuffer_width() && py < framebuffer_height()) {
                framebuffer_put_pixel(px, py, COLOR_WHITE);
            }
        }
        for (i32 x = cx - 4; x <= cx + 4; x++) {
            const u32 pxt = (u32)(base_x + x);
            const u32 pyt = (u32)(base_y + (cy - 6));
            const u32 pxb = (u32)(base_x + x);
            const u32 pyb = (u32)(base_y + (cy + 6));
            if (pxt < framebuffer_width() && pyt < framebuffer_height()) framebuffer_put_pixel(pxt, pyt, COLOR_WHITE);
            if (pxb < framebuffer_width() && pyb < framebuffer_height()) framebuffer_put_pixel(pxb, pyb, COLOR_WHITE);
        }
    } else if (g_cursor_state == CURSOR_DRAG) {
        /* 4-way drag cross */
        for (i32 d = -6; d <= 6; d++) {
            const u32 px = (u32)(base_x + (cx + d));
            const u32 py = (u32)(base_y + cy);
            if (px < framebuffer_width() && py < framebuffer_height()) framebuffer_put_pixel(px, py, COLOR_WHITE);
        }
        for (i32 d = -6; d <= 6; d++) {
            const u32 px = (u32)(base_x + cx);
            const u32 py = (u32)(base_y + (cy + d));
            if (px < framebuffer_width() && py < framebuffer_height()) framebuffer_put_pixel(px, py, COLOR_WHITE);
        }
        /* Tips */
        {
            const u32 upx = (u32)(base_x + cx);
            const u32 upy = (u32)(base_y + (cy - 7));
            const u32 dnx = (u32)(base_x + cx);
            const u32 dny = (u32)(base_y + (cy + 7));
            const u32 lfx = (u32)(base_x + (cx - 7));
            const u32 lfy = (u32)(base_y + cy);
            const u32 rtx = (u32)(base_x + (cx + 7));
            const u32 rty = (u32)(base_y + cy);
            if (upx < framebuffer_width() && upy < framebuffer_height()) framebuffer_put_pixel(upx, upy, COLOR_WHITE);
            if (dnx < framebuffer_width() && dny < framebuffer_height()) framebuffer_put_pixel(dnx, dny, COLOR_WHITE);
            if (lfx < framebuffer_width() && lfy < framebuffer_height()) framebuffer_put_pixel(lfx, lfy, COLOR_WHITE);
            if (rtx < framebuffer_width() && rty < framebuffer_height()) framebuffer_put_pixel(rtx, rty, COLOR_WHITE);
        }
    }
}

void ui_events_init(void) {
    log_write("ui: event dispatch initialized");
}

void ui_cursor_init(void) {
    g_cur_x = 64;
    g_cur_y = 64;
    g_cur_last_x = g_cur_x;
    g_cur_last_y = g_cur_y;
    g_cur_visible = false;
    g_mouse_left = false;
    g_cursor_state = CURSOR_DEFAULT;
    kmemset(g_cur_backing, 0, sizeof(g_cur_backing));

    if (!framebuffer_ready()) {
        log_write("ui: cursor disabled (no framebuffer)");
        return;
    }

    cursor_clamp();
    cursor_save();
    cursor_draw();
    g_cur_visible = true;
    /* Present once so the initial desktop appears even before the first input event. */
    framebuffer_present();
    framebuffer_damage_clear();
    log_write("ui: cursor initialized");
}

void ui_events_pump(void) {
    input_event_t evt;
    while (input_event_pop(&evt)) {
        if (evt.type == INPUT_EVENT_MOUSE_MOVE) {
            cursor_restore();
            g_cur_x += evt.a;
            g_cur_y += evt.b;
            cursor_clamp();
            if (startscreen_is_open()) {
                ui_cursor_set_state(CURSOR_DEFAULT);
            } else if (apps_has_active_window()) {
                (void)apps_handle_mouse(&evt, g_cur_x, g_cur_y);
                ui_cursor_set_state(apps_cursor_state(g_cur_x, g_cur_y, g_mouse_left));
            } else {
                desktop_handle_input(&evt, g_cur_x, g_cur_y);
                ui_cursor_set_state(CURSOR_DEFAULT);
            }
            /* Only redraw menu bar / dock when the pointer is near them, or when overlays are open. */
            bool need_bar = startmenu_needs_redraw() || notifications_needs_redraw() || dock_needs_redraw();
            if (!need_bar && framebuffer_ready()) {
                const i32 h = (i32)framebuffer_height();
                if (g_cur_y < 40) {
                    need_bar = true;
                } else if (g_cur_y > h - 110) {
                    need_bar = true;
                }
            }
            if (need_bar) {
                taskbar_init();
            }
            ui_cursor_end_overlay();
        } else if (evt.type == INPUT_EVENT_MOUSE_BUTTON) {
            /* Keep a visual cursor even if the first interaction is a click. */
            if (!g_cur_visible) {
                ui_cursor_end_overlay();
            }
        }

        if (evt.type == INPUT_EVENT_MOUSE_BUTTON) {
            g_mouse_left = evt.a ? true : false;
            ui_cursor_begin_overlay();
            bool consumed = false;
            if (startscreen_is_open()) {
                consumed = startscreen_handle_input(&evt, g_cur_x, g_cur_y);
                ui_cursor_set_state(CURSOR_DEFAULT);
            }
            if (!consumed) {
                /* Dock/top bar overlays should always get first shot. */
                if (dock_handle_input(&evt, g_cur_x, g_cur_y)) {
                    consumed = true;
                    ui_cursor_set_state(CURSOR_DEFAULT);
                }
            }
            if (!consumed && apps_has_active_window()) {
                consumed = apps_handle_mouse(&evt, g_cur_x, g_cur_y);
                ui_cursor_set_state(apps_cursor_state(g_cur_x, g_cur_y, g_mouse_left));
            }
            if (!consumed) {
                desktop_handle_input(&evt, g_cur_x, g_cur_y);
                ui_cursor_set_state(CURSOR_DEFAULT);
            }
            taskbar_init();
            ui_cursor_end_overlay();
        }

        if (evt.type == INPUT_EVENT_KEY) {
            ui_cursor_begin_overlay();
            if (startscreen_is_open()) {
                startscreen_handle_input(&evt, g_cur_x, g_cur_y);
            } else if (apps_has_active_window()) {
                apps_handle_key(evt.a, evt.b ? true : false);
            } else {
                desktop_handle_key(evt.a, evt.b ? true : false);
            }
            taskbar_init();
            ui_cursor_end_overlay();
        }

        /* Event processing hook for window manager and desktop widgets. */
    }
}
