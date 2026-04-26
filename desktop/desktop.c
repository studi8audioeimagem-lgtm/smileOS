#include "kernel.h"
#include "../gfx/colors.h"
#include "icons.h"

typedef enum {
    APP_ICON_NOTEPAD = 0,
    APP_ICON_TERMINAL,
    APP_ICON_SYSCONFIG
} app_icon_id_t;

typedef struct {
    app_icon_id_t id;
    u32 x;
    u32 y;
    u32 w;
    u32 h;
    u32 color;
    const char *label;
    const char *glyph;
} desktop_icon_t;

static desktop_icon_t g_icons[3];
static bool g_left_down;
static bool g_search_active;
static char g_search[48];
static usize g_search_len;
static bool g_right_down;
static i32 g_hover_idx;
static u64 g_last_click_tick;
static i32 g_last_click_idx;
static bool g_ctx_open;
static i32 g_ctx_x;
static i32 g_ctx_y;
static bool g_selected[3];

/* Desktop marquee selection (drag select). */
static bool g_marquee_active;
static i32 g_marquee_sx;
static i32 g_marquee_sy;
static i32 g_marquee_cx;
static i32 g_marquee_cy;

static u32 col_panel(void) {
    os_settings_t *s = settings_get();
    return s->dark_theme ? COLOR_PANEL : 0xE9EEF5;
}

static u32 col_border(void) {
    os_settings_t *s = settings_get();
    return s->dark_theme ? COLOR_PANEL_BORDER : 0xB5C0D0;
}

static u32 col_tile(void) {
    os_settings_t *s = settings_get();
    return s->dark_theme ? COLOR_TILE : 0xF7F9FC;
}

static u32 col_text(void) {
    os_settings_t *s = settings_get();
    return s->dark_theme ? COLOR_WHITE : COLOR_BLACK;
}

static u32 lerp_rgb(u32 a, u32 b, u32 t255) {
    const u32 ar = (a >> 16) & 0xFF;
    const u32 ag = (a >> 8) & 0xFF;
    const u32 ab = a & 0xFF;
    const u32 br = (b >> 16) & 0xFF;
    const u32 bg = (b >> 8) & 0xFF;
    const u32 bb = b & 0xFF;
    const u32 r = (ar * (255U - t255) + br * t255) / 255U;
    const u32 g = (ag * (255U - t255) + bg * t255) / 255U;
    const u32 b2 = (ab * (255U - t255) + bb * t255) / 255U;
    return (r << 16) | (g << 8) | b2;
}

static u32 hash32(u32 x) {
    x ^= x >> 16;
    x *= 0x7FEB352DU;
    x ^= x >> 15;
    x *= 0x846CA68BU;
    x ^= x >> 16;
    return x;
}

static u32 *g_wallpaper_cache;
static u32 g_wp_w;
static u32 g_wp_h;

static u32 clamp8(i32 v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (u32)v;
}

static u32 add_rgb(u32 c, i32 dr, i32 dg, i32 db) {
    const i32 r = (i32)((c >> 16) & 0xFF) + dr;
    const i32 g = (i32)((c >> 8) & 0xFF) + dg;
    const i32 b = (i32)(c & 0xFF) + db;
    return (clamp8(r) << 16) | (clamp8(g) << 8) | clamp8(b);
}

static void wallpaper_build_cache(void) {
    if (!framebuffer_ready()) {
        return;
    }
    const u32 w = framebuffer_width();
    const u32 h = framebuffer_height();
    if (w == 0 || h == 0) {
        return;
    }

    const usize need = (usize)w * (usize)h;
    if (!g_wallpaper_cache || g_wp_w != w || g_wp_h != h) {
        g_wallpaper_cache = (u32 *)kmalloc(need * sizeof(u32));
        g_wp_w = w;
        g_wp_h = h;
    }
    if (!g_wallpaper_cache) {
        return;
    }

    os_settings_t *s = settings_get();

    /* Detailed wallpaper: layered gradient + noise + soft light blobs + vignette. */
    const u32 top = s->dark_theme ? 0x071022 : 0xEAF2FF;
    const u32 mid = s->dark_theme ? 0x0E1A34 : 0xD9F0FF;
    const u32 bot = s->dark_theme ? 0x04070F : 0xF9FBFF;

    const i32 cx = (i32)(w / 2);
    const i32 cy = (i32)(h / 2);
    const i32 maxd2 = cx * cx + cy * cy;

    const i32 b1x = (i32)(w / 4);
    const i32 b1y = (i32)(h / 5);
    const i32 b2x = (i32)(w * 7 / 10);
    const i32 b2y = (i32)(h * 6 / 10);

    for (u32 y = 0; y < h; y++) {
        const u32 t = (y * 255U) / (h ? h : 1);
        const u32 base = (t < 160) ? lerp_rgb(top, mid, (t * 255U) / 160U)
                                   : lerp_rgb(mid, bot, ((t - 160U) * 255U) / 95U);

        for (u32 x = 0; x < w; x++) {
            u32 c = base;

            /* Subtle grain */
            const u32 n = hash32((x + 1U) * 2654435761U ^ (y + 7U) * 2246822519U);
            const i32 noise = (i32)((n >> 28) & 0x0F) - 7; /* -7..8 */
            c = add_rgb(c, noise, noise, noise);

            /* Vignette */
            const i32 dx = (i32)x - cx;
            const i32 dy = (i32)y - cy;
            const i32 d2 = dx * dx + dy * dy;
            const i32 vig = maxd2 ? (d2 * 110) / maxd2 : 0; /* 0..110 */
            c = add_rgb(c, -vig / 3, -vig / 3, -vig / 2);

            /* Cool highlight blob */
            {
                const i32 ddx = (i32)x - b1x;
                const i32 ddy = (i32)y - b1y;
                const u32 bd2 = (u32)(ddx * ddx + ddy * ddy);
                const u32 k = w * 38U;
                const u32 a = (k * 255U) / (k + (bd2 ? bd2 : 1U));
                c = lerp_rgb(c, s->dark_theme ? 0x184CFF : 0x7FC7FF, a / 6U);
            }

            /* Warm accent blob */
            {
                const i32 ddx = (i32)x - b2x;
                const i32 ddy = (i32)y - b2y;
                const u32 bd2 = (u32)(ddx * ddx + ddy * ddy);
                const u32 k = w * 28U;
                const u32 a = (k * 255U) / (k + (bd2 ? bd2 : 1U));
                c = lerp_rgb(c, s->dark_theme ? 0xB24BFF : 0xFFA6CF, a / 8U);
            }

            g_wallpaper_cache[(usize)y * (usize)w + (usize)x] = c;
        }
    }
}

void desktop_draw_wallpaper_rect(u32 x, u32 y, u32 width, u32 height) {
    if (!framebuffer_ready()) {
        return;
    }
    const u32 sw = framebuffer_width();
    const u32 sh = framebuffer_height();
    if (sw == 0 || sh == 0 || width == 0 || height == 0) {
        return;
    }
    if (x >= sw || y >= sh) {
        return;
    }
    if (!g_wallpaper_cache || g_wp_w != sw || g_wp_h != sh) {
        wallpaper_build_cache();
    }
    u32 w = width;
    u32 h = height;
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (g_wallpaper_cache) {
        const u32 *src = g_wallpaper_cache + (usize)y * (usize)sw + (usize)x;
        framebuffer_blit_rect(x, y, w, h, src, sw);
    } else {
        framebuffer_fill_rect_fast(x, y, w, h, 0x0B1220);
    }
}

static void draw_wallpaper(void) {
    desktop_draw_wallpaper_rect(0, 0, framebuffer_width(), framebuffer_height());
}

static void draw_app_icon(const desktop_icon_t *ic) {
    if (!ic || !framebuffer_ready()) {
        return;
    }

    draw_rect(ic->x, ic->y, ic->w, ic->h, col_border());
    draw_rect(ic->x + 2, ic->y + 2, ic->w - 4, ic->h - 4, col_tile());

    icon_id_t iid = ICON_NOTEPAD;
    if (ic->id == APP_ICON_NOTEPAD) iid = ICON_NOTEPAD;
    else if (ic->id == APP_ICON_TERMINAL) iid = ICON_TERMINAL;
    else if (ic->id == APP_ICON_SYSCONFIG) iid = ICON_SETTINGS;

    const i32 sz = (i32)ic->w - 10;
    const i32 ix = (i32)ic->x + 5;
    const i32 iy = (i32)ic->y + 5;
    icon_draw(iid, ix, iy, sz, false);

    /* Label (uppercase) */
    text_draw(ic->x, ic->y + ic->h + 8, ic->label, col_text());
}

static void draw_outline_rect(u32 x, u32 y, u32 w, u32 h, u32 color) {
    if (!framebuffer_ready() || w == 0 || h == 0) return;
    draw_rect(x, y, w, 1, color);
    if (h > 1) draw_rect(x, y + h - 1, w, 1, color);
    if (h > 2) {
        draw_rect(x, y + 1, 1, h - 2, color);
        if (w > 1) draw_rect(x + w - 1, y + 1, 1, h - 2, color);
    }
}

static void marquee_norm(i32 *x0, i32 *y0, i32 *x1, i32 *y1) {
    i32 ax0 = g_marquee_sx;
    i32 ay0 = g_marquee_sy;
    i32 ax1 = g_marquee_cx;
    i32 ay1 = g_marquee_cy;
    if (ax1 < ax0) { i32 t = ax0; ax0 = ax1; ax1 = t; }
    if (ay1 < ay0) { i32 t = ay0; ay0 = ay1; ay1 = t; }
    if (x0) *x0 = ax0;
    if (y0) *y0 = ay0;
    if (x1) *x1 = ax1;
    if (y1) *y1 = ay1;
}

static void marquee_update_selection(void) {
    if (!g_marquee_active) return;
    i32 x0, y0, x1, y1;
    marquee_norm(&x0, &y0, &x1, &y1);
    if (x1 - x0 < 2 || y1 - y0 < 2) {
        for (u32 i = 0; i < 3; i++) g_selected[i] = false;
        return;
    }

    for (u32 i = 0; i < 3; i++) {
        const i32 ix0 = (i32)g_icons[i].x;
        const i32 iy0 = (i32)g_icons[i].y;
        const i32 ix1 = (i32)g_icons[i].x + (i32)g_icons[i].w;
        const i32 iy1 = (i32)g_icons[i].y + (i32)g_icons[i].h;
        const bool hit = !(x1 <= ix0 || ix1 <= x0 || y1 <= iy0 || iy1 <= y0);
        g_selected[i] = hit;
    }
}

static void draw_marquee_overlay(void) {
    if (!framebuffer_ready() || !g_marquee_active) {
        return;
    }
    i32 x0, y0, x1, y1;
    marquee_norm(&x0, &y0, &x1, &y1);
    if (x1 - x0 < 2 || y1 - y0 < 2) {
        return;
    }

    /* Clamp to screen. */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    const i32 sw = (i32)framebuffer_width();
    const i32 sh = (i32)framebuffer_height();
    if (x1 > sw) x1 = sw;
    if (y1 > sh) y1 = sh;
    if (x1 <= x0 || y1 <= y0) return;

    const u32 w = (u32)(x1 - x0);
    const u32 h = (u32)(y1 - y0);
    const u32 accent = settings_accent_color();

    /* Transparent fill + crisp outline. */
    draw_rect_alpha((u32)x0, (u32)y0, w, h, accent, 40);
    draw_outline_rect((u32)x0, (u32)y0, w, h, accent);
}

static void setup_icons(void) {
    os_settings_t *s = settings_get();
    const u32 icon_w = s->large_icons ? 104 : 72;
    const u32 icon_h = s->large_icons ? 104 : 72;
    const u32 start_x = 28;
    const u32 start_y = 44;
    const u32 gap = s->large_icons ? 34 : 26;

    g_icons[0].id = APP_ICON_NOTEPAD;
    g_icons[0].x = start_x;
    g_icons[0].y = start_y;
    g_icons[0].w = icon_w;
    g_icons[0].h = icon_h;
    g_icons[0].color = settings_accent_color();
    g_icons[0].label = "NOTEPAD";
    g_icons[0].glyph = "N";

    g_icons[1].id = APP_ICON_TERMINAL;
    g_icons[1].x = start_x + (icon_w + gap);
    g_icons[1].y = start_y;
    g_icons[1].w = icon_w;
    g_icons[1].h = icon_h;
    g_icons[1].color = s->dark_theme ? 0x0B0E14 : 0x2A2F3A;
    g_icons[1].label = "BASH\nTERMINAL";
    g_icons[1].glyph = "B";

    g_icons[2].id = APP_ICON_SYSCONFIG;
    g_icons[2].x = start_x + 2 * (icon_w + gap);
    g_icons[2].y = start_y;
    g_icons[2].w = icon_w;
    g_icons[2].h = icon_h;
    g_icons[2].color = s->dark_theme ? COLOR_SUCCESS : 0x1E8A56;
    g_icons[2].label = "SYSCONFIG";
    g_icons[2].glyph = "S";
}

static void vga_write_banner(const char *text) {
    volatile unsigned short *vga = (volatile unsigned short *)0xB8000;
    const unsigned short attr = 0x0F00;
    usize i = 0;

    while (text[i] != 0) {
        vga[i] = attr | (unsigned char)text[i];
        i++;
    }
}

static void draw_search_bar(void) {
    if (!framebuffer_ready() || !g_search_active) {
        return;
    }

    const u32 w = framebuffer_width();
    const u32 bar_w = (w > 560) ? 520 : (w - 28);
    const u32 bar_x = (w - bar_w) / 2;
    const u32 bar_y = 14;

    draw_shadow_rect(bar_x, bar_y, bar_w, 42);
    draw_rect(bar_x, bar_y, bar_w, 42, col_border());
    draw_rect_alpha(bar_x + 1, bar_y + 1, bar_w - 2, 40, col_panel(), 235);
    text_draw(bar_x + 12, bar_y + 14, g_search, col_text());
}

static void draw_tutorial_overlay(void) {
    os_settings_t *s = settings_get();
    if (!framebuffer_ready() || !s->tutorial_enabled || s->tutorial_completed) {
        return;
    }

    const u32 w = framebuffer_width();
    const u32 h = framebuffer_height();
    const u32 box_w = (w > 560) ? 520 : (w - 28);
    const u32 box_x = (w - box_w) / 2;
    const u32 box_y = 74;
    const u32 box_h = 112;

    draw_rect_alpha(0, 0, w, h, COLOR_BLACK, 60);
    draw_shadow_rect(box_x, box_y, box_w, box_h);
    draw_rect(box_x, box_y, box_w, box_h, col_border());
    draw_rect_alpha(box_x + 1, box_y + 1, box_w - 2, box_h - 2, col_panel(), 235);

    const char *title = "WELCOME TO SMILEOS";
    text_draw(box_x + 14, box_y + 12, title, col_text());

    const char *line1 = 0;
    const char *line2 = 0;
    if (s->tutorial_step == 0) {
        line1 = "STEP 1: MOVE THE MOUSE";
        line2 = "TRY MOVING THE CURSOR A LITTLE.";
    } else if (s->tutorial_step == 1) {
        line1 = "STEP 2: OPEN NOTEPAD";
        line2 = "CLICK THE NOTEPAD ICON ON THE DESKTOP.";
    } else if (s->tutorial_step == 2) {
        line1 = "STEP 3: TYPE SOMETHING";
        line2 = "TYPE ON THE KEYBOARD IN NOTEPAD.";
    } else if (s->tutorial_step == 3) {
        line1 = "STEP 4: CLOSE THE WINDOW";
        line2 = "CLICK THE X BUTTON OR PRESS ESC.";
    } else {
        line1 = "TUTORIAL COMPLETE";
        line2 = "HAVE FUN!";
    }

    if (line1) text_draw(box_x + 14, box_y + 40, line1, col_text());
    if (line2) text_draw(box_x + 14, box_y + 56, line2, col_text());
    text_draw(box_x + 14, box_y + 84, "TIP: PRESS / TO SEARCH APPS", COLOR_SUCCESS);
}

static void draw_context_menu(void) {
    if (!framebuffer_ready() || !g_ctx_open) {
        return;
    }

    const u32 w = 220;
    const u32 h = 132;
    u32 sw = framebuffer_width();
    u32 sh = framebuffer_height();
    u32 x = (g_ctx_x < 0) ? 0U : (u32)g_ctx_x;
    u32 y = (g_ctx_y < 0) ? 0U : (u32)g_ctx_y;
    if (x + w + 4 > sw) x = (sw > (w + 4)) ? (sw - w - 4) : 0;
    if (y + h + 4 > sh) y = (sh > (h + 4)) ? (sh - h - 4) : 0;

    draw_shadow_rect(x, y, w, h);
    draw_rect(x, y, w, h, col_border());
    draw_rect_alpha(x + 1, y + 1, w - 2, h - 2, col_panel(), 245);
    text_draw(x + 12, y + 14, "DESKTOP MENU", col_text());
    draw_rect(x + 10, y + 32, w - 20, 1, col_border());

    draw_rect_alpha(x + 10, y + 44, w - 20, 24, col_tile(), 210);
    text_draw(x + 16, y + 52, "TOGGLE THEME", col_text());

    draw_rect_alpha(x + 10, y + 74, w - 20, 24, col_tile(), 210);
    text_draw(x + 16, y + 82, "TOGGLE ICONS", col_text());

    draw_rect_alpha(x + 10, y + 104, w - 20, 24, col_tile(), 210);
    text_draw(x + 16, y + 112, "OPEN SYSCONFIG", col_text());
}

static void desktop_redraw_internal(void) {
    if (!framebuffer_ready()) {
        return;
    }
    draw_wallpaper();
    setup_icons();
    for (u32 i = 0; i < 3; i++) {
        if (g_selected[i]) {
            /* Selection highlight behind hover highlight. */
            draw_rect_alpha(g_icons[i].x - 4, g_icons[i].y - 4, g_icons[i].w + 8, g_icons[i].h + 8, settings_accent_color(), 55);
        }
        if (g_hover_idx == (i32)i) {
            draw_rect(g_icons[i].x - 3, g_icons[i].y - 3, g_icons[i].w + 6, g_icons[i].h + 6, settings_accent_color());
        }
        draw_app_icon(&g_icons[i]);
    }
    draw_marquee_overlay();
    draw_search_bar();
    draw_tutorial_overlay();
    draw_context_menu();
    framebuffer_damage_full();
}

void desktop_redraw(void) {
    desktop_redraw_internal();
}

static bool rects_intersect(i32 ax, i32 ay, i32 aw, i32 ah, i32 bx, i32 by, i32 bw, i32 bh) {
    if (aw <= 0 || ah <= 0 || bw <= 0 || bh <= 0) return false;
    if (ax + aw <= bx) return false;
    if (bx + bw <= ax) return false;
    if (ay + ah <= by) return false;
    if (by + bh <= ay) return false;
    return true;
}

void desktop_redraw_rect(i32 x, i32 y, i32 width, i32 height) {
    if (!framebuffer_ready()) {
        return;
    }

    /* If we have global overlays, do a full redraw to avoid leaving artifacts. */
    os_settings_t *s = settings_get();
    if ((s->tutorial_enabled && !s->tutorial_completed) || g_ctx_open || g_search_active) {
        desktop_redraw_internal();
        return;
    }

    /* Clamp */
    i32 x0 = x;
    i32 y0 = y;
    i32 x1 = x + width;
    i32 y1 = y + height;
    if (width <= 0 || height <= 0) return;
    if (x1 <= 0 || y1 <= 0) return;
    const i32 sw = (i32)framebuffer_width();
    const i32 sh = (i32)framebuffer_height();
    if (x0 >= sw || y0 >= sh) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > sw) x1 = sw;
    if (y1 > sh) y1 = sh;
    if (x1 <= x0 || y1 <= y0) return;

    const u32 rw = (u32)(x1 - x0);
    const u32 rh = (u32)(y1 - y0);
    desktop_draw_wallpaper_rect((u32)x0, (u32)y0, rw, rh);

    setup_icons();

    /* Redraw any desktop icons that intersect the region. */
    for (u32 i = 0; i < 3; i++) {
        const i32 ix = (i32)g_icons[i].x - 3;
        const i32 iy = (i32)g_icons[i].y - 3;
        const i32 iw = (i32)g_icons[i].w + 6;
        const i32 ih = (i32)g_icons[i].h + 32; /* include label area */
        if (!rects_intersect(x0, y0, (i32)rw, (i32)rh, ix, iy, iw, ih)) {
            continue;
        }
        if (g_hover_idx == (i32)i) {
            draw_rect(g_icons[i].x - 3, g_icons[i].y - 3, g_icons[i].w + 6, g_icons[i].h + 6, settings_accent_color());
        }
        if (g_selected[i]) {
            draw_rect_alpha(g_icons[i].x - 4, g_icons[i].y - 4, g_icons[i].w + 8, g_icons[i].h + 8, settings_accent_color(), 55);
        }
        draw_app_icon(&g_icons[i]);
    }

    /* Marquee overlay if it intersects the damaged region. */
    if (g_marquee_active) {
        i32 mx0, my0, mx1, my1;
        marquee_norm(&mx0, &my0, &mx1, &my1);
        const i32 mw = mx1 - mx0;
        const i32 mh = my1 - my0;
        if (rects_intersect(x0, y0, (i32)rw, (i32)rh, mx0, my0, mw, mh)) {
            draw_marquee_overlay();
        }
    }

    framebuffer_damage_rect(x0, y0, (i32)rw, (i32)rh);
}

void desktop_init(void) {
    if (framebuffer_ready()) {
        wallpaper_build_cache();
        desktop_redraw_internal();
    } else {
        vga_write_banner("smileOS booted (text fallback)");
    }
    g_left_down = false;
    g_right_down = false;
    g_hover_idx = -1;
    g_last_click_tick = 0;
    g_last_click_idx = -1;
    g_ctx_open = false;
    g_search_active = false;
    g_search_len = 0;
    g_search[0] = 0;
    for (u32 i = 0; i < 3; i++) g_selected[i] = false;
    g_marquee_active = false;
    g_marquee_sx = g_marquee_sy = g_marquee_cx = g_marquee_cy = 0;
    log_write("desktop: wallpaper + icon grid initialized");
}

static bool pt_in_icon(const desktop_icon_t *ic, i32 x, i32 y) {
    if (!ic) {
        return false;
    }
    if (x < (i32)ic->x || y < (i32)ic->y) {
        return false;
    }
    if (x >= (i32)(ic->x + ic->w) || y >= (i32)(ic->y + ic->h)) {
        return false;
    }
    return true;
}

void desktop_handle_input(const input_event_t *evt, i32 cursor_x, i32 cursor_y) {
    if (!evt || !framebuffer_ready()) {
        return;
    }

    os_settings_t *s = settings_get();

    if (evt->type == INPUT_EVENT_MOUSE_MOVE) {
        /* Keep desktop hover/tips off while an app window is visible. */
        if (apps_has_active_window()) {
            return;
        }

        /* Marquee drag update. */
        if (g_marquee_active && g_left_down) {
            i32 ox0, oy0, ox1, oy1;
            marquee_norm(&ox0, &oy0, &ox1, &oy1);
            const i32 old_w = ox1 - ox0;
            const i32 old_h = oy1 - oy0;

            g_marquee_cx = cursor_x;
            g_marquee_cy = cursor_y;
            marquee_update_selection();

            i32 nx0, ny0, nx1, ny1;
            marquee_norm(&nx0, &ny0, &nx1, &ny1);
            const i32 new_w = nx1 - nx0;
            const i32 new_h = ny1 - ny0;

            /* Redraw union of old+new marquee rect (with padding for outline). */
            const i32 pad = 6;
            i32 ux0 = ox0;
            i32 uy0 = oy0;
            i32 ux1 = ox0 + old_w;
            i32 uy1 = oy0 + old_h;
            if (nx0 < ux0) ux0 = nx0;
            if (ny0 < uy0) uy0 = ny0;
            if (nx0 + new_w > ux1) ux1 = nx0 + new_w;
            if (ny0 + new_h > uy1) uy1 = ny0 + new_h;

            desktop_redraw_rect(ux0 - pad, uy0 - pad, (ux1 - ux0) + pad * 2, (uy1 - uy0) + pad * 2);
            return;
        }

        if (s->tutorial_enabled && !s->tutorial_completed && s->tutorial_step == 0) {
            s->tutorial_step = 1;
            desktop_redraw();
        }

        i32 new_hover = -1;
        for (u32 i = 0; i < 3; i++) {
            if (pt_in_icon(&g_icons[i], cursor_x, cursor_y)) {
                new_hover = (i32)i;
                break;
            }
        }
        if (new_hover != g_hover_idx) {
            g_hover_idx = new_hover;
            desktop_redraw();
        }
        return;
    }

    if (evt->type != INPUT_EVENT_MOUSE_BUTTON) {
        return;
    }

    const bool left = evt->a ? true : false;
    const bool right = evt->b ? true : false;
    const bool rising_left = left && !g_left_down;
    const bool rising_right = right && !g_right_down;
    g_left_down = left;
    g_right_down = right;

    /* Finish marquee on mouse release. */
    if (!left && g_marquee_active) {
        i32 mx0, my0, mx1, my1;
        marquee_norm(&mx0, &my0, &mx1, &my1);
        const i32 pad = 6;
        g_marquee_active = false;
        desktop_redraw_rect(mx0 - pad, my0 - pad, (mx1 - mx0) + pad * 2, (my1 - my0) + pad * 2);
        return;
    }

    if (rising_right) {
        /* Cancel marquee if user right-clicks while dragging/selecting. */
        g_marquee_active = false;
        g_ctx_open = !g_ctx_open;
        g_ctx_x = cursor_x;
        g_ctx_y = cursor_y;
        desktop_redraw();
        return;
    }

    if (!rising_left) {
        return;
    }

    /* Context menu click handling. */
    if (g_ctx_open) {
        const i32 x = (g_ctx_x < 0) ? 0 : g_ctx_x;
        const i32 y = (g_ctx_y < 0) ? 0 : g_ctx_y;
        if (cursor_x >= x + 10 && cursor_x < x + 210) {
            if (cursor_y >= y + 44 && cursor_y < y + 68) {
                s->dark_theme = !s->dark_theme;
                notifications_post("THEME CHANGED");
                g_ctx_open = false;
                desktop_redraw();
                return;
            }
            if (cursor_y >= y + 74 && cursor_y < y + 98) {
                s->large_icons = !s->large_icons;
                notifications_post("ICON SIZE CHANGED");
                g_ctx_open = false;
                desktop_redraw();
                return;
            }
            if (cursor_y >= y + 104 && cursor_y < y + 128) {
                g_ctx_open = false;
                desktop_redraw();
                apps_open_sysconfig();
                return;
            }
        }
        g_ctx_open = false;
        desktop_redraw();
        return;
    }

    /* Desktop icons (single click opens; this is a beginner-friendly OS). */
    /* If the click is on empty background, start a marquee selection drag. */
    bool hit_icon = false;
    for (u32 i = 0; i < 3; i++) {
        if (!pt_in_icon(&g_icons[i], cursor_x, cursor_y)) {
            continue;
        }
        hit_icon = true;

        if (g_icons[i].id == APP_ICON_NOTEPAD) {
            if (s->tutorial_enabled && !s->tutorial_completed && s->tutorial_step == 1) {
                s->tutorial_step = 2;
            }
            notifications_post("OPENING NOTEPAD");
            apps_open_notepad();
        } else if (g_icons[i].id == APP_ICON_TERMINAL) {
            notifications_post("OPENING TERMINAL");
            apps_open_bash_terminal();
        } else if (g_icons[i].id == APP_ICON_SYSCONFIG) {
            notifications_post("OPENING SYSCONFIG");
            apps_open_sysconfig();
        }
        return;
    }

    if (!hit_icon && !g_ctx_open && !g_search_active &&
        !(s->tutorial_enabled && !s->tutorial_completed)) {
        for (u32 i = 0; i < 3; i++) g_selected[i] = false;
        g_hover_idx = -1;
        g_marquee_active = true;
        g_marquee_sx = cursor_x;
        g_marquee_sy = cursor_y;
        g_marquee_cx = cursor_x;
        g_marquee_cy = cursor_y;
        marquee_update_selection();
        desktop_redraw_rect(cursor_x - 8, cursor_y - 8, 16, 16);
        return;
    }
}

static bool is_printable(i32 keycode) {
    return keycode >= 32 && keycode <= 255;
}

static bool str_contains_case_insensitive(const char *hay, const char *needle) {
    if (!hay || !needle) return false;
    if (needle[0] == 0) return true;

    for (usize i = 0; hay[i] != 0; i++) {
        usize j = 0;
        for (;;) {
            const char a = hay[i + j];
            const char b = needle[j];
            if (b == 0) return true;
            if (a == 0) break;

            char al = a;
            char bl = b;
            if (al >= 'A' && al <= 'Z') al = (char)(al + 32);
            if (bl >= 'A' && bl <= 'Z') bl = (char)(bl + 32);
            if (al != bl) break;
            j++;
        }
    }
    return false;
}

static void open_best_match(void) {
    /* Only 3 apps right now: do a simple match over labels. */
    if (str_contains_case_insensitive("NOTEPAD", g_search) || g_search_len == 0) {
        apps_open_notepad();
        return;
    }
    if (str_contains_case_insensitive("BASH TERMINAL", g_search) || str_contains_case_insensitive("TERMINAL", g_search)) {
        apps_open_bash_terminal();
        return;
    }
    if (str_contains_case_insensitive("FILES", g_search) || str_contains_case_insensitive("EXPLORER", g_search)) {
        apps_open_files();
        return;
    }
    if (str_contains_case_insensitive("CALC", g_search) || str_contains_case_insensitive("CALCULATOR", g_search)) {
        apps_open_calculator();
        return;
    }
    if (str_contains_case_insensitive("CLOCK", g_search) || str_contains_case_insensitive("TIME", g_search)) {
        apps_open_clock();
        return;
    }
    if (str_contains_case_insensitive("PALETTE", g_search) || str_contains_case_insensitive("COLORS", g_search)) {
        apps_open_palette();
        return;
    }
    if (str_contains_case_insensitive("TYPING", g_search)) {
        apps_open_typing();
        return;
    }
    if (str_contains_case_insensitive("HELP", g_search)) {
        apps_open_help();
        return;
    }
    if (str_contains_case_insensitive("ABOUT", g_search)) {
        apps_open_about();
        return;
    }
    if (str_contains_case_insensitive("SYSCONFIG", g_search) || str_contains_case_insensitive("SETTINGS", g_search)) {
        apps_open_sysconfig();
        return;
    }
    /* Fallback: open nothing. */
}

void desktop_handle_key(i32 keycode, bool pressed) {
    if (!pressed || !framebuffer_ready()) {
        return;
    }

    if (keycode == KEY_ESC) {
        g_search_active = false;
        g_search_len = 0;
        g_search[0] = 0;
        desktop_redraw();
        return;
    }

    if (!g_search_active) {
        if (keycode == '/' || is_printable(keycode)) {
            g_search_active = true;
            g_search_len = 0;
            g_search[0] = 0;
            if (is_printable(keycode) && keycode != '/') {
                g_search[0] = (char)keycode;
                g_search[1] = 0;
                g_search_len = 1;
            }
            desktop_redraw();
        }
        return;
    }

    if (keycode == KEY_BACKSPACE) {
        if (g_search_len) {
            g_search_len--;
            g_search[g_search_len] = 0;
        }
        desktop_redraw();
        return;
    }

    if (keycode == '\n') {
        g_search_active = false;
        desktop_redraw();
        open_best_match();
        return;
    }

    if (is_printable(keycode)) {
        if (g_search_len + 1 < sizeof(g_search)) {
            g_search[g_search_len++] = (char)keycode;
            g_search[g_search_len] = 0;
            desktop_redraw();
        }
        return;
    }
}
