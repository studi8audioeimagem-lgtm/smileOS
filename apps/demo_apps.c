#include "kernel.h"
#include "../gfx/colors.h"
#include "string.h"
#include "fat32.h"
#include "icons.h"
#include "../net/net.h"

typedef enum {
    APP_NONE = 0,
    APP_NOTEPAD,
    APP_TERMINAL,
    APP_SYSCONFIG,
    APP_CALC,
    APP_CLOCK,
    APP_FILES,
    APP_PALETTE,
    APP_TYPING,
    APP_HELP,
    APP_ABOUT,
    APP_TRASH,
    APP_BROWSER
} active_app_t;

typedef struct {
    u32 x;
    u32 y;
    u32 w;
    u32 h;
} app_window_t;

static active_app_t g_active;
static app_window_t g_win;
static bool g_left_down;
static bool g_dragging;
static i32 g_drag_off_x;
static i32 g_drag_off_y;
static bool g_minimized;
static bool g_maximized;
static app_window_t g_restore;
static bool g_animating;
static bool g_anim_opening;
static u64 g_anim_tick;

static char g_note_buf[2048];
static usize g_note_len;
static usize g_note_cursor;

static char g_term_line[256];
static usize g_term_len;
static usize g_term_cursor;
static char g_term_hist[32][128];
static usize g_term_hist_count;

static char g_calc_line[64];
static usize g_calc_len;
static i32 g_calc_last;
static bool g_calc_has_last;

static char g_type_buf[64];
static usize g_type_len;

static bool g_fat_mounted;
static fat32_dirent_t g_dir[64];
static usize g_dir_count;
static i32 g_dir_sel;

static char g_web_addr[64];
static usize g_web_addr_len;
static usize g_web_addr_cursor;
static char g_web_body[4096];
static usize g_web_body_len;
static i32 g_web_scroll;
static bool g_web_connected;
static char g_web_router_name[48];
static usize g_web_router_name_len;
static usize g_web_router_name_cursor;
static char g_web_router_pass[48];
static usize g_web_router_pass_len;
static usize g_web_router_pass_cursor;
static u8 g_web_focus; /* 0=name,1=pass,2=addr */

static bool pt_in_rect(i32 x, i32 y, i32 rx, i32 ry, i32 rw, i32 rh) {
    if (rw <= 0 || rh <= 0) return false;
    if (x < rx || y < ry) return false;
    if (x >= rx + rw || y >= ry + rh) return false;
    return true;
}

static bool pt_in_window(i32 x, i32 y) {
    return pt_in_rect(x, y, (i32)g_win.x, (i32)g_win.y, (i32)g_win.w, (i32)g_win.h);
}

static u32 col_border(void) {
    os_settings_t *s = settings_get();
    return s->dark_theme ? COLOR_PANEL_BORDER : 0xB5C0D0;
}

static u32 col_panel(void) {
    os_settings_t *s = settings_get();
    return s->dark_theme ? COLOR_PANEL : 0xE9EEF5;
}

static u32 col_text(void) {
    os_settings_t *s = settings_get();
    return s->dark_theme ? COLOR_WHITE : COLOR_BLACK;
}

static u32 col_window_bg(void) {
    os_settings_t *s = settings_get();
    return s->dark_theme ? COLOR_WINDOW_BG : 0xFFFFFF;
}

static u32 col_accent(void) {
    return settings_accent_color();
}

static void draw_char(u32 x, u32 y, char ch, u32 color) {
    char s[2];
    s[0] = ch;
    s[1] = 0;
    text_draw(x, y, s, color);
}

static void draw_window_chrome(u32 x, u32 y, u32 w, u32 h, const char *title) {
    if (!framebuffer_ready() || w < 120 || h < 80) {
        return;
    }

    const u32 r = 12;

    /* macOS-ish: rounded window + soft shadow + translucent title bar. */
    /* Blur what's behind the window for a "glass" look. */
    if (w > 6 && h > 6) {
        draw_blur_rect(x + 3, y + 3, w - 6, h - 6, 4);
    }
    draw_shadow_round_rect(x, y, w, h, r);
    draw_round_rect(x, y, w, h, r, col_border());
    draw_round_rect_alpha(x + 1, y + 1, w - 2, h - 2, r - 1, col_window_bg(), 232);

    /* Title bar */
    draw_round_rect_alpha(x + 1, y + 1, w - 2, 34, r - 1, col_panel(), 185);
    draw_rect_alpha(x + 1, y + 1, w - 2, 10, COLOR_WHITE, 18);
    draw_rect(x + 1, y + 34, w - 2, 1, col_border());

    text_draw(x + 84, y + 13, title, COLOR_WHITE);

    /* Traffic light buttons (close/min/max) */
    const u32 cy = y + 17;
    draw_round_rect(x + 14, cy - 6, 12, 12, 6, 0xFF5F57); /* red */
    draw_round_rect(x + 34, cy - 6, 12, 12, 6, 0xFEBC2E); /* yellow */
    draw_round_rect(x + 54, cy - 6, 12, 12, 6, 0x28C840); /* green */
}

static void note_draw(void) {
    const u32 x = g_win.x;
    const u32 y = g_win.y;
    const u32 w = g_win.w;
    const u32 h = g_win.h;

    draw_window_chrome(x, y, w, h, "NOTEPAD");
    {
        os_settings_t *s = settings_get();
        if (s->tutorial_enabled && !s->tutorial_completed) {
            if (s->tutorial_step == 2) {
                text_draw(x + 120, y + 11, "TUTORIAL: TYPE SOMETHING", COLOR_SUCCESS);
            } else if (s->tutorial_step == 3) {
                text_draw(x + 120, y + 11, "TUTORIAL: CLOSE WITH X OR ESC", COLOR_SUCCESS);
            }
        }
    }

    const u32 pad_x = x + 10;
    const u32 pad_y = y + 44;
    const u32 pad_w = w - 20;
    const u32 pad_h = h - 54;
    draw_rect(pad_x, pad_y, pad_w, pad_h, COLOR_WHITE);
    draw_rect(pad_x + 2, pad_y + 2, pad_w - 4, pad_h - 4, 0xF4F6FA);

    /* Text area (baked 20px font; monospaced cell metrics from text module) */
    const u32 text_x = pad_x + 10;
    const u32 text_y = pad_y + 10;
    const u32 text_w = (pad_w > 20) ? (pad_w - 20) : 0;
    const u32 text_h = (pad_h > 20) ? (pad_h - 20) : 0;
    const u32 adv = text_cell_advance();
    const u32 line_h = text_line_height();
    const u32 max_cols = (adv == 0) ? 0 : (text_w / adv);
    const u32 max_rows = (line_h == 0) ? 0 : (text_h / line_h);

    u32 cx = 0;
    u32 cy = 0;
    u32 caret_px = text_x;
    u32 caret_py = text_y;

    for (usize i = 0; i <= g_note_len; i++) {
        if (i == g_note_cursor) {
            caret_px = text_x + cx * adv;
            caret_py = text_y + cy * line_h;
        }
        if (i == g_note_len) {
            break;
        }

        char ch = g_note_buf[i];
        if (ch == '\n') {
            cx = 0;
            cy++;
            if (cy >= max_rows) {
                break;
            }
            continue;
        }

        if (max_cols != 0 && cx >= max_cols) {
            cx = 0;
            cy++;
            if (cy >= max_rows) {
                break;
            }
        }

        draw_char(text_x + cx * adv, text_y + cy * line_h, ch, 0x0B0E14);
        cx++;
    }

    /* Blinking caret */
    if (((scheduler_ticks() / 25ULL) & 1ULL) == 0ULL) {
        const u32 ch = (line_h > 2) ? (line_h - 2) : line_h;
        draw_rect(caret_px, caret_py + 1, 2, ch, col_accent());
    }
}

static void term_draw(void) {
    const u32 x = g_win.x;
    const u32 y = g_win.y;
    const u32 w = g_win.w;
    const u32 h = g_win.h;

    draw_window_chrome(x, y, w, h, "BASH TERMINAL");
    {
        os_settings_t *s = settings_get();
        if (s->tutorial_enabled && !s->tutorial_completed) {
            if (s->tutorial_step == 3) {
                text_draw(x + 160, y + 11, "TUTORIAL: CLOSE WITH X OR ESC", COLOR_SUCCESS);
            }
        }
    }

    const u32 pad_x = x + 10;
    const u32 pad_y = y + 44;
    const u32 pad_w = w - 20;
    const u32 pad_h = h - 54;
    draw_rect(pad_x, pad_y, pad_w, pad_h, 0x0B0E14);
    draw_rect(pad_x, pad_y, pad_w, 1, COLOR_PANEL_BORDER);

    const u32 text_x = pad_x + 10;
    const u32 text_y = pad_y + 10;
    const u32 adv = text_cell_advance();
    const u32 line_h = text_line_height();

    const u32 max_rows = (pad_h > 28) ? ((pad_h - 28) / line_h) : 0;
    const u32 hist_rows = (max_rows > 1) ? (max_rows - 1) : 0; /* keep 1 row for prompt */
    const usize start = (g_term_hist_count > hist_rows) ? (g_term_hist_count - hist_rows) : 0;

    u32 row = 0;
    for (usize i = start; i < g_term_hist_count && row < hist_rows; i++, row++) {
        text_draw(text_x, text_y + row * line_h, g_term_hist[i], COLOR_WHITE);
    }

    const char *prompt = "SMILEOS:/ $ ";
    text_draw(text_x, text_y + row * line_h, prompt, COLOR_SUCCESS);

    u32 prompt_cols = 0;
    for (usize i = 0; prompt[i] != 0; i++) {
        prompt_cols++;
    }
    for (usize i = 0; i < g_term_len; i++) {
        draw_char(text_x + (prompt_cols + (u32)i) * adv, text_y + row * line_h, g_term_line[i], COLOR_WHITE);
    }

    if (((scheduler_ticks() / 25ULL) & 1ULL) == 0ULL) {
        const u32 caret_x = text_x + (prompt_cols + (u32)g_term_cursor) * adv;
        const u32 caret_y = text_y + row * line_h;
        const u32 ch = (line_h > 2) ? (line_h - 2) : line_h;
        draw_rect(caret_x, caret_y + 1, 2, ch, COLOR_WHITE);
    }
}

static void trash_draw(void) {
    const u32 x = g_win.x;
    const u32 y = g_win.y;
    const u32 w = g_win.w;
    const u32 h = g_win.h;

    draw_window_chrome(x, y, w, h, "TRASH");

    const u32 pad_x = x + 10;
    const u32 pad_y = y + 44;
    const u32 pad_w = w - 20;
    const u32 pad_h = h - 54;

    draw_round_rect_alpha(pad_x, pad_y, pad_w, pad_h, 12, col_panel(), 235);
    draw_round_rect(pad_x, pad_y, pad_w, pad_h, 12, col_border());

    text_draw(pad_x + 16, pad_y + 16, "TRASH IS EMPTY (PLACEHOLDER)", col_text());
    text_draw(pad_x + 16, pad_y + 36, "DELETE/RESTORE WILL BE ADDED LATER.", col_text());
}

static void web_strip_to_body(const char *src, char *dst, usize cap, usize *out_len) {
    if (!dst || cap == 0) return;
    if (out_len) *out_len = 0;
    if (!src) { dst[0] = 0; return; }

    /* Skip HTTP headers if present. */
    const char *p = src;
    for (usize i = 0; p[i] != 0; i++) {
        if (p[i] == '\r' && p[i + 1] == '\n' && p[i + 2] == '\r' && p[i + 3] == '\n') {
            p = &p[i + 4];
            break;
        }
    }

    /* Very naive HTML strip: drop anything between < and >. */
    bool in_tag = false;
    usize o = 0;
    for (usize i = 0; p[i] != 0 && o + 1 < cap; i++) {
        const char ch = p[i];
        if (ch == '<') { in_tag = true; continue; }
        if (ch == '>') { in_tag = false; continue; }
        if (in_tag) continue;
        if (ch == '\r') continue;
        /* Collapse tabs */
        dst[o++] = (ch == '\t') ? ' ' : ch;
    }
    dst[o] = 0;
    if (out_len) *out_len = o;
}

static bool parse_ipv4(const char *s, u32 *out_ip_host) {
    if (!s || !out_ip_host) return false;
    u32 parts[4] = {0,0,0,0};
    u32 pi = 0;
    u32 v = 0;
    bool have = false;
    for (usize i = 0;; i++) {
        const char ch = s[i];
        if (ch >= '0' && ch <= '9') {
            v = v * 10U + (u32)(ch - '0');
            if (v > 255U) return false;
            have = true;
            continue;
        }
        if (ch == '.' || ch == 0) {
            if (!have) return false;
            if (pi >= 4) return false;
            parts[pi++] = v;
            v = 0;
            have = false;
            if (ch == 0) break;
            continue;
        }
        return false;
    }
    if (pi != 4) return false;
    *out_ip_host = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

static void web_draw(void) {
    const u32 x = g_win.x;
    const u32 y = g_win.y;
    const u32 w = g_win.w;
    const u32 h = g_win.h;

    draw_window_chrome(x, y, w, h, "WEB");

    const u32 pad_x = x + 10;
    const u32 pad_y = y + 44;
    const u32 pad_w = w - 20;
    const u32 pad_h = h - 54;
    draw_round_rect_alpha(pad_x, pad_y, pad_w, pad_h, 12, col_panel(), 235);
    draw_round_rect(pad_x, pad_y, pad_w, pad_h, 12, col_border());

    const net_state_t *ns = net_state();
    g_web_connected = (ns && ns->up);

    /* If not connected, show a simple "router connect" UI first. */
    if (!g_web_connected) {
        const u32 title_y = pad_y + 18;
        text_draw(pad_x + 18, title_y, "ROUTER", COLOR_WHITE);

        /* Router icon */
        icon_draw(ICON_WIFI, (i32)pad_x + 18, (i32)pad_y + 40, 44, false);

        const u32 box_x = pad_x + 74;
        const u32 box_w = pad_w - 94;
        const u32 row_h = 30;

        /* Name */
        draw_round_rect_alpha(box_x, pad_y + 44, box_w, row_h, 10, COLOR_TILE, 160);
        draw_round_rect(box_x, pad_y + 44, box_w, row_h, 10, col_border());
        text_draw(box_x + 10, pad_y + 52, g_web_router_name, COLOR_WHITE);

        /* Password */
        draw_round_rect_alpha(box_x, pad_y + 80, box_w, row_h, 10, COLOR_TILE, 160);
        draw_round_rect(box_x, pad_y + 80, box_w, row_h, 10, col_border());
        /* Mask password with '*' */
        char stars[48];
        kmemset(stars, 0, sizeof(stars));
        for (usize i = 0; i < g_web_router_pass_len && i + 1 < sizeof(stars); i++) stars[i] = '*';
        stars[(g_web_router_pass_len < sizeof(stars)) ? g_web_router_pass_len : (sizeof(stars) - 1)] = 0;
        text_draw(box_x + 10, pad_y + 88, stars, COLOR_WHITE);

        /* Connect button */
        const u32 btn_w = 160;
        const u32 btn_h = 34;
        const u32 btn_x = box_x;
        const u32 btn_y = pad_y + 124;
        draw_round_rect(btn_x, btn_y, btn_w, btn_h, 12, settings_accent_color());
        text_draw(btn_x + 46, btn_y + 12, "CONNECT", COLOR_WHITE);

        /* Caret in focused field */
        if (((scheduler_ticks() / 25ULL) & 1ULL) == 0ULL) {
            const u32 adv = text_cell_advance();
            if (g_web_focus == 0) {
                const u32 cx = box_x + 10 + (u32)g_web_router_name_cursor * adv;
                draw_rect(cx, pad_y + 50, 2, row_h - 10, COLOR_WHITE);
            } else if (g_web_focus == 1) {
                const u32 cx = box_x + 10 + (u32)g_web_router_pass_cursor * adv;
                draw_rect(cx, pad_y + 86, 2, row_h - 10, COLOR_WHITE);
            }
        }

        text_draw(pad_x + 18, pad_y + pad_h - 18, "TIP: TAB SWITCHES FIELDS. ENTER CONNECTS.", COLOR_WHITE);
        return;
    }

    /* Address bar (connected mode) */
    const u32 bar_h = 32;
    draw_round_rect_alpha(pad_x + 10, pad_y + 10, pad_w - 20, bar_h, 10, COLOR_TILE, 160);
    draw_round_rect(pad_x + 10, pad_y + 10, pad_w - 20, bar_h, 10, col_border());
    text_draw(pad_x + 18, pad_y + 18, g_web_addr, COLOR_WHITE);

    if (((scheduler_ticks() / 25ULL) & 1ULL) == 0ULL) {
        const u32 adv = text_cell_advance();
        const u32 cx = pad_x + 18 + (u32)g_web_addr_cursor * adv;
        draw_rect(cx, pad_y + 16, 2, bar_h - 12, COLOR_WHITE);
    }

    /* Content */
    const u32 cx0 = pad_x + 14;
    const u32 cy0 = pad_y + 52;
    const u32 cw = pad_w - 28;
    const u32 ch = pad_h - 62;
    draw_rect_alpha(cx0, cy0, cw, ch, COLOR_BLACK, 35);

    /* Render as plain text. */
    const u32 line_h = text_line_height();
    const u32 max_lines = (line_h ? (ch / line_h) : 0);

    u32 line = 0;
    u32 col = 0;
    u32 start_idx = 0;
    u32 cur_line = 0;

    /* Find start index based on scroll (lines). */
    for (usize i = 0; g_web_body[i] != 0; i++) {
        if (cur_line >= (u32)g_web_scroll) {
            start_idx = (u32)i;
            break;
        }
        if (g_web_body[i] == '\n') cur_line++;
    }

    char rowbuf[128];
    kmemset(rowbuf, 0, sizeof(rowbuf));
    usize ri = 0;
    for (usize i = start_idx; g_web_body[i] != 0 && line < max_lines; i++) {
        const char chh = g_web_body[i];
        if (chh == '\n' || ri + 1 >= sizeof(rowbuf)) {
            rowbuf[ri] = 0;
            text_draw(cx0 + 10, cy0 + 10 + line * line_h, rowbuf, COLOR_WHITE);
            kmemset(rowbuf, 0, sizeof(rowbuf));
            ri = 0;
            line++;
            col = 0;
            continue;
        }
        (void)col;
        rowbuf[ri++] = chh;
        col++;
    }
}

typedef enum {
    SETTINGS_APPEARANCE = 0,
    SETTINGS_DOCK,
    SETTINGS_WALLPAPER,
    SETTINGS_INPUT,
    SETTINGS_NETWORK,
    SETTINGS_ABOUT
} settings_page_t;

static settings_page_t g_settings_page;

static void settings_draw(void) {
    const u32 x = g_win.x;
    const u32 y = g_win.y;
    const u32 w = g_win.w;
    const u32 h = g_win.h;

    draw_window_chrome(x, y, w, h, "SYSTEM SETTINGS");
    {
        os_settings_t *s = settings_get();
        if (s->tutorial_enabled && !s->tutorial_completed && s->tutorial_step == 1) {
            text_draw(x + 140, y + 11, "TUTORIAL: CLICK NOTEPAD ON DESKTOP", COLOR_SUCCESS);
        }
    }

    const u32 pad_x = x + 10;
    const u32 pad_y = y + 44;
    const u32 pad_w = w - 20;
    const u32 pad_h = h - 54;

    draw_rect_alpha(pad_x, pad_y, pad_w, pad_h, col_panel(), 235);

    os_settings_t *s = settings_get();

    /* Sidebar */
    const u32 side_w = 170;
    draw_rect_alpha(pad_x, pad_y, side_w, pad_h, col_panel(), 245);
    draw_rect(pad_x + side_w, pad_y, 1, pad_h, col_border());

    const char *items[] = {"APPEARANCE", "DOCK", "WALLPAPER", "KEYBOARD & MOUSE", "NETWORK", "ABOUT"};
    for (u32 i = 0; i < 6; i++) {
        const u32 iy = pad_y + 14 + i * 30;
        if ((u32)g_settings_page == i) {
            draw_round_rect_alpha(pad_x + 10, iy - 6, side_w - 20, 24, 10, col_accent(), 70);
        }
        text_draw(pad_x + 18, iy, items[i], col_text());
    }

    /* Content area */
    const u32 cx = pad_x + side_w + 14;
    const u32 cy = pad_y + 14;
    const u32 cw = pad_w - side_w - 28;

    if (g_settings_page == SETTINGS_APPEARANCE) {
        text_draw(cx, cy, "THEME", col_text());
        draw_rect(cx, cy + 16, cw, 1, col_border());

        /* Theme buttons */
        const char *t0 = "LIGHT";
        const char *t1 = "DARK";
        const char *t2 = "AQUA";
        const char *t3 = "GRAPHITE";
        const u32 bw = 110;
        const u32 bh = 28;
        const u32 by = cy + 30;
        const u32 gap = 10;

        const u32 tx0 = cx;
        const u32 tx1 = cx + bw + gap;
        const u32 tx2 = cx + 2 * (bw + gap);
        const u32 tx3 = cx + 3 * (bw + gap);

        const u32 sel = s->theme_mode;
        draw_round_rect_alpha(tx0, by, bw, bh, 10, (sel == 0) ? col_accent() : col_panel(), 220);
        text_draw(tx0 + 14, by + 10, t0, COLOR_WHITE);
        draw_round_rect_alpha(tx1, by, bw, bh, 10, (sel == 1) ? col_accent() : col_panel(), 220);
        text_draw(tx1 + 18, by + 10, t1, COLOR_WHITE);
        draw_round_rect_alpha(tx2, by, bw, bh, 10, (sel == 2) ? col_accent() : col_panel(), 220);
        text_draw(tx2 + 18, by + 10, t2, COLOR_WHITE);
        draw_round_rect_alpha(tx3, by, bw, bh, 10, (sel == 3) ? col_accent() : col_panel(), 220);
        text_draw(tx3 + 10, by + 10, t3, COLOR_WHITE);

        text_draw(cx, by + 48, "ACCENT", col_text());
        draw_rect(cx, by + 64, cw, 1, col_border());

        /* Accent swatches */
        for (u32 i = 0; i < 10; i++) {
            const u32 col = palette_get((u16)(i * 997));
            const u32 sx = cx + i * 26;
            const u32 sy = by + 78;
            draw_round_rect(sx, sy, 18, 18, 6, col);
            if (s->accent_color == col) {
                draw_round_rect(sx - 2, sy - 2, 22, 22, 7, COLOR_WHITE);
            }
        }
    } else if (g_settings_page == SETTINGS_DOCK) {
        text_draw(cx, cy, "DOCK", col_text());
        draw_rect(cx, cy + 16, cw, 1, col_border());

        text_draw(cx, cy + 34, "AUTO-HIDE", col_text());
        draw_round_rect_alpha(cx + 160, cy + 30, 54, 22, 10, s->dock_autohide ? col_accent() : col_border(), 220);
        if (s->dock_autohide) {
            draw_round_rect(cx + 192, cy + 32, 18, 18, 9, COLOR_WHITE);
        } else {
            draw_round_rect(cx + 164, cy + 32, 18, 18, 9, COLOR_WHITE);
        }

        text_draw(cx, cy + 68, "DO NOT DISTURB", col_text());
        draw_round_rect_alpha(cx + 160, cy + 64, 54, 22, 10, s->dnd ? col_accent() : col_border(), 220);
        if (s->dnd) {
            draw_round_rect(cx + 192, cy + 66, 18, 18, 9, COLOR_WHITE);
        } else {
            draw_round_rect(cx + 164, cy + 66, 18, 18, 9, COLOR_WHITE);
        }
    } else if (g_settings_page == SETTINGS_WALLPAPER) {
        text_draw(cx, cy, "WALLPAPER", col_text());
        draw_rect(cx, cy + 16, cw, 1, col_border());
        text_draw(cx, cy + 34, "SOFT GRADIENT (DEFAULT)", col_text());
        text_draw(cx, cy + 50, "MORE OPTIONS LATER", col_text());
    } else if (g_settings_page == SETTINGS_INPUT) {
        text_draw(cx, cy, "KEYBOARD & MOUSE", col_text());
        draw_rect(cx, cy + 16, cw, 1, col_border());
        text_draw(cx, cy + 34, "PT-BR LAYOUT (DEFAULT)", col_text());
        text_draw(cx, cy + 50, "MOUSE: PS/2 + USB (BASIC)", col_text());
    } else if (g_settings_page == SETTINGS_NETWORK) {
        text_draw(cx, cy, "NETWORK", col_text());
        draw_rect(cx, cy + 16, cw, 1, col_border());
        text_draw(cx, cy + 34, "NOT IMPLEMENTED", col_text());
    } else {
        text_draw(cx, cy, "ABOUT THIS OS", col_text());
        draw_rect(cx, cy + 16, cw, 1, col_border());
        text_draw(cx, cy + 34, "SMILEOS - EARLY BETA", col_text());
        text_draw(cx, cy + 50, "UI DEMO + FAT32 READER", col_text());
    }
}

static void calc_draw(void) {
    const u32 x = g_win.x;
    const u32 y = g_win.y;
    const u32 w = g_win.w;
    const u32 h = g_win.h;

    draw_window_chrome(x, y, w, h, "CALCULATOR");

    const u32 pad_x = x + 10;
    const u32 pad_y = y + 44;
    const u32 pad_w = w - 20;
    const u32 pad_h = h - 54;

    draw_rect_alpha(pad_x, pad_y, pad_w, pad_h, col_panel(), 235);
    text_draw(pad_x + 12, pad_y + 14, "EXPR:", col_text());
    text_draw(pad_x + 68, pad_y + 14, g_calc_line, COLOR_WHITE);

    if (g_calc_has_last) {
        char rbuf[24];
        kmemset(rbuf, 0, sizeof(rbuf));
        rbuf[0] = 'R'; rbuf[1] = 'E'; rbuf[2] = 'S'; rbuf[3] = ':'; rbuf[4] = ' ';
        /* signed int to decimal */
        i32 v = g_calc_last;
        char tmp[16];
        usize ti = 0;
        bool neg = false;
        if (v < 0) { neg = true; v = -v; }
        do {
            tmp[ti++] = (char)('0' + (v % 10));
            v /= 10;
        } while (v && ti < sizeof(tmp));
        usize o = 5;
        if (neg && o < sizeof(rbuf) - 1) rbuf[o++] = '-';
        while (ti && o < sizeof(rbuf) - 1) {
            rbuf[o++] = tmp[--ti];
        }
        rbuf[o] = 0;
        text_draw(pad_x + 12, pad_y + 40, rbuf, col_text());
    }

    text_draw(pad_x + 12, pad_y + 70, "TYPE: 12+34 ENTER", col_text());
    text_draw(pad_x + 12, pad_y + 86, "SHORTCUTS: + - * /", col_text());
}

static void clock_draw(void) {
    const u32 x = g_win.x;
    const u32 y = g_win.y;
    const u32 w = g_win.w;
    const u32 h = g_win.h;

    draw_window_chrome(x, y, w, h, "CLOCK");

    const u32 pad_x = x + 10;
    const u32 pad_y = y + 44;
    const u32 pad_w = w - 20;
    const u32 pad_h = h - 54;
    draw_rect_alpha(pad_x, pad_y, pad_w, pad_h, col_panel(), 235);

    /* Uptime display. */
    const u64 ticks = scheduler_ticks();
    const u32 secs = (u32)(ticks / 50ULL);
    const u32 mm = (secs / 60U) % 100U;
    const u32 ss = secs % 60U;
    char buf[16];
    buf[0] = (char)('0' + (mm / 10U));
    buf[1] = (char)('0' + (mm % 10U));
    buf[2] = ':';
    buf[3] = (char)('0' + (ss / 10U));
    buf[4] = (char)('0' + (ss % 10U));
    buf[5] = 0;
    text_draw_scaled(pad_x + 12, pad_y + 18, buf, col_accent(), 3);
    text_draw(pad_x + 12, pad_y + 54, "UPTIME (MM:SS)", col_text());
}

static void palette_draw(void) {
    const u32 x = g_win.x;
    const u32 y = g_win.y;
    const u32 w = g_win.w;
    const u32 h = g_win.h;

    draw_window_chrome(x, y, w, h, "PALETTE");
    const u32 pad_x = x + 10;
    const u32 pad_y = y + 44;
    const u32 pad_w = w - 20;
    const u32 pad_h = h - 54;
    draw_rect_alpha(pad_x, pad_y, pad_w, pad_h, col_panel(), 235);

    /* Show ~500 swatches (25 x 20) for a "more colors" feel without being too dense. */
    u32 cols = 25;
    u32 rows = 20;
    const u32 gap = 2;
    const u32 margin_x = 12;
    const u32 margin_y = 12;
    const u32 bottom_pad = 24;

    u32 avail_w = (pad_w > (margin_x * 2 + (cols - 1) * gap)) ? (pad_w - margin_x * 2 - (cols - 1) * gap) : 0;
    u32 avail_h = (pad_h > (margin_y * 2 + bottom_pad + (rows - 1) * gap)) ? (pad_h - margin_y * 2 - bottom_pad - (rows - 1) * gap) : 0;
    u32 cell_w = (cols && avail_w) ? (avail_w / cols) : 0;
    u32 cell_h = (rows && avail_h) ? (avail_h / rows) : 0;
    u32 cell = (cell_w < cell_h) ? cell_w : cell_h;
    if (cell < 6) cell = 6;
    if (cell > 18) cell = 18;

    /* If the window is tiny, reduce density but keep "hundreds" of colors. */
    if (pad_w < 520 || pad_h < 300) {
        cols = 20;
        rows = 15;
    }

    u16 idx = 0;
    for (u32 ry = 0; ry < rows; ry++) {
        for (u32 cx = 0; cx < cols; cx++) {
            const u32 px = pad_x + margin_x + cx * (cell + gap);
            const u32 py = pad_y + margin_y + ry * (cell + gap);
            if (px + cell >= pad_x + pad_w || py + cell >= pad_y + pad_h - bottom_pad) {
                continue;
            }
            draw_rect(px, py, cell, cell, palette_get(idx++));
        }
    }

    text_draw(pad_x + 12, pad_y + pad_h - 18, "500+ COLORS (OF 10,000)", col_text());
}

static void typing_draw(void) {
    const u32 x = g_win.x;
    const u32 y = g_win.y;
    const u32 w = g_win.w;
    const u32 h = g_win.h;
    draw_window_chrome(x, y, w, h, "TYPING PRACTICE");

    const u32 pad_x = x + 10;
    const u32 pad_y = y + 44;
    const u32 pad_w = w - 20;
    const u32 pad_h = h - 54;
    draw_rect_alpha(pad_x, pad_y, pad_w, pad_h, col_panel(), 235);
    const char *target = "hello smileos";
    text_draw(pad_x + 12, pad_y + 14, "TARGET:", col_text());
    text_draw(pad_x + 80, pad_y + 14, target, COLOR_WHITE);
    text_draw(pad_x + 12, pad_y + 34, "YOU:", col_text());

    /* Show typed text; green when correct prefix, red when wrong. */
    bool ok = true;
    for (usize i = 0; i < g_type_len; i++) {
        if (target[i] == 0 || g_type_buf[i] != target[i]) {
            ok = false;
            break;
        }
    }
    text_draw(pad_x + 58, pad_y + 34, g_type_buf, ok ? COLOR_SUCCESS : COLOR_ERROR);
    text_draw(pad_x + 12, pad_y + 56, "BACKSPACE TO FIX", col_text());
}

static void help_draw(void) {
    const u32 x = g_win.x;
    const u32 y = g_win.y;
    const u32 w = g_win.w;
    const u32 h = g_win.h;
    draw_window_chrome(x, y, w, h, "HELP");

    const u32 pad_x = x + 10;
    const u32 pad_y = y + 44;
    const u32 pad_w = w - 20;
    const u32 pad_h = h - 54;
    draw_rect_alpha(pad_x, pad_y, pad_w, pad_h, col_panel(), 235);
    text_draw(pad_x + 12, pad_y + 14, "MOUSE: CLICK ICONS TO OPEN", col_text());
    text_draw(pad_x + 12, pad_y + 30, "DESKTOP: / SEARCH APPS", col_text());
    text_draw(pad_x + 12, pad_y + 46, "WINDOW: DRAG TITLE BAR", col_text());
    text_draw(pad_x + 12, pad_y + 62, "KEYS: ESC CLOSES WINDOW", col_text());
    text_draw(pad_x + 12, pad_y + 78, "TASKBAR: CLICK START", col_text());
}

static void about_draw(void) {
    const u32 x = g_win.x;
    const u32 y = g_win.y;
    const u32 w = g_win.w;
    const u32 h = g_win.h;
    draw_window_chrome(x, y, w, h, "ABOUT");

    const u32 pad_x = x + 10;
    const u32 pad_y = y + 44;
    const u32 pad_w = w - 20;
    const u32 pad_h = h - 54;
    draw_rect_alpha(pad_x, pad_y, pad_w, pad_h, col_panel(), 235);
    text_draw(pad_x + 12, pad_y + 14, "SMILEOS", col_text());
    text_draw(pad_x + 12, pad_y + 30, "EARLY BETA BUILD", col_text());
    text_draw(pad_x + 12, pad_y + 46, "UI DEMO + INPUT STACK", col_text());
}

static void files_draw(void) {
    const u32 x = g_win.x;
    const u32 y = g_win.y;
    const u32 w = g_win.w;
    const u32 h = g_win.h;
    draw_window_chrome(x, y, w, h, "FILES (FAT32)");

    const u32 pad_x = x + 10;
    const u32 pad_y = y + 44;
    const u32 pad_w = w - 20;
    const u32 pad_h = h - 54;
    draw_rect_alpha(pad_x, pad_y, pad_w, pad_h, col_panel(), 235);

    if (!g_fat_mounted) {
        text_draw(pad_x + 12, pad_y + 14, "NO FAT32 DISK FOUND.", col_text());
        text_draw(pad_x + 12, pad_y + 30, "QEMU: ATTACH AN IDE FAT32 IMAGE.", col_text());
        return;
    }

    text_draw(pad_x + 12, pad_y + 12, "ROOT DIRECTORY", col_text());
    draw_rect(pad_x + 12, pad_y + 28, pad_w - 24, 1, col_border());

    const u32 row_h = 16;
    u32 ry = pad_y + 38;
    for (usize i = 0; i < g_dir_count; i++) {
        if (ry + row_h >= pad_y + pad_h) break;
        if ((i32)i == g_dir_sel) {
            draw_rect_alpha(pad_x + 10, ry - 2, pad_w - 20, row_h + 4, col_accent(), 80);
        }
        text_draw(pad_x + 14, ry, g_dir[i].name, COLOR_WHITE);
        ry += row_h;
    }
    text_draw(pad_x + 12, pad_y + pad_h - 18, "CLICK A FILE TO OPEN (TXT)", col_text());
}

static void apps_redraw_active(void) {
    if (g_minimized) {
        return;
    }
    app_window_t saved = g_win;
    app_window_t drawn = g_win;

    if (g_animating) {
        const u64 now = scheduler_ticks();
        u64 dt = now - g_anim_tick;
        if (dt > 14) dt = 14;
        u32 prog = (u32)(dt * 255 / 14);
        if (!g_anim_opening) {
            prog = 255 - prog;
        }
        /* Scale 0.88 -> 1.0 and offset to center. */
        const u32 scale = 225U + (prog * 31U) / 255U; /* /256 */
        const u32 aw = (g_win.w * scale) / 256U;
        const u32 ah = (g_win.h * scale) / 256U;
        const u32 ax = g_win.x + (g_win.w - aw) / 2;
        const u32 ay = g_win.y + (g_win.h - ah) / 2;
        g_win.x = ax;
        g_win.y = ay;
        g_win.w = aw;
        g_win.h = ah;
        drawn = g_win;

        if (dt >= 14) {
            g_animating = false;
        }
    }
    if (g_active == APP_NOTEPAD) {
        note_draw();
    } else if (g_active == APP_TERMINAL) {
        term_draw();
    } else if (g_active == APP_SYSCONFIG) {
        settings_draw();
    } else if (g_active == APP_CALC) {
        calc_draw();
    } else if (g_active == APP_CLOCK) {
        clock_draw();
    } else if (g_active == APP_FILES) {
        files_draw();
    } else if (g_active == APP_PALETTE) {
        palette_draw();
    } else if (g_active == APP_TYPING) {
        typing_draw();
    } else if (g_active == APP_HELP) {
        help_draw();
    } else if (g_active == APP_ABOUT) {
        about_draw();
    } else if (g_active == APP_TRASH) {
        trash_draw();
    } else if (g_active == APP_BROWSER) {
        web_draw();
    }

    /* Mark the window area as damaged so the cursor overlay can present it efficiently. */
    {
        const i32 m = 28; /* shadow + safety */
        framebuffer_damage_rect((i32)drawn.x - m, (i32)drawn.y - m, (i32)drawn.w + m * 2, (i32)drawn.h + m * 2);
    }

    g_win = saved;
}

bool apps_has_active_window(void) {
    return g_active != APP_NONE && !g_minimized;
}

bool apps_has_running_app(void) {
    return g_active != APP_NONE;
}

const char *apps_running_title(void) {
    switch (g_active) {
        case APP_NOTEPAD: return "NOTEPAD";
        case APP_TERMINAL: return "TERMINAL";
        case APP_SYSCONFIG: return "SYSCONFIG";
        case APP_CALC: return "CALC";
        case APP_CLOCK: return "CLOCK";
        case APP_FILES: return "FILES";
        case APP_PALETTE: return "PALETTE";
        case APP_TYPING: return "TYPING";
        case APP_HELP: return "HELP";
        case APP_ABOUT: return "ABOUT";
        case APP_TRASH: return "TRASH";
        case APP_BROWSER: return "WEB";
        default: return 0;
    }
}

void apps_restore_minimized(void) {
    if (g_active == APP_NONE) {
        return;
    }
    if (!g_minimized) {
        return;
    }
    g_minimized = false;
    desktop_redraw();
    apps_redraw_active();
}

void apps_minimize_active(void) {
    if (g_active == APP_NONE) {
        return;
    }
    /* Animate out, then actually minimize. */
    g_animating = true;
    g_anim_opening = false;
    g_anim_tick = scheduler_ticks();
    g_dragging = false;
    /* We'll flip g_minimized when the animation ends via apps_needs_redraw(). */
    desktop_redraw();
    log_write("apps: minimized");
}

static void win_clamp_to_screen(void) {
    if (!framebuffer_ready()) {
        return;
    }
    const i32 max_x = (i32)framebuffer_width() - (i32)g_win.w;
    const i32 max_y = (i32)framebuffer_height() - (i32)g_win.h;
    if ((i32)g_win.x < 0) g_win.x = 0;
    if ((i32)g_win.y < 0) g_win.y = 0;
    if ((i32)g_win.x > max_x) g_win.x = (u32)((max_x < 0) ? 0 : max_x);
    if ((i32)g_win.y > max_y) g_win.y = (u32)((max_y < 0) ? 0 : max_y);
}

static void apps_close_active(void) {
    if (g_active == APP_NONE) {
        return;
    }
    g_active = APP_NONE;
    g_dragging = false;
    g_minimized = false;
    g_maximized = false;

    os_settings_t *s = settings_get();
    if (s->tutorial_enabled && !s->tutorial_completed && s->tutorial_step == 3) {
        s->tutorial_completed = true;
        s->tutorial_step = 4;
        log_write("tutorial: completed");
    }

    /* Redraw desktop behind the closed window. */
    desktop_redraw();
    log_write("apps: active window closed");
}

static void notepad_set_text(const char *s, u32 len) {
    if (!s) {
        return;
    }
    if (len > sizeof(g_note_buf) - 1) {
        len = (u32)sizeof(g_note_buf) - 1;
    }
    kmemset(g_note_buf, 0, sizeof(g_note_buf));
    for (u32 i = 0; i < len; i++) {
        g_note_buf[i] = (char)s[i];
    }
    g_note_len = len;
    g_note_cursor = g_note_len;
}

void apps_open_notepad(void) {
    if (!framebuffer_ready()) {
        return;
    }
    log_write("apps: open notepad");

    const u32 w = framebuffer_width();
    const u32 h = framebuffer_height();
    const u32 win_w = (w > 640) ? 560 : (w - 40);
    const u32 win_h = (h > 480) ? 380 : (h - 90);

    g_win.x = (w - win_w) / 2;
    g_win.y = (h - win_h) / 3;
    g_win.w = win_w;
    g_win.h = win_h;
    g_active = APP_NOTEPAD;
    g_dragging = false;
    g_minimized = false;
    g_maximized = false;
    g_animating = true;
    g_anim_opening = true;
    g_anim_tick = scheduler_ticks();

    apps_redraw_active();
}

void apps_open_bash_terminal(void) {
    if (!framebuffer_ready()) {
        return;
    }
    log_write("apps: open terminal");

    const u32 w = framebuffer_width();
    const u32 h = framebuffer_height();
    const u32 win_w = (w > 720) ? 620 : (w - 40);
    const u32 win_h = (h > 520) ? 360 : (h - 90);

    g_win.x = (w - win_w) / 2;
    g_win.y = (h - win_h) / 4;
    g_win.w = win_w;
    g_win.h = win_h;
    g_active = APP_TERMINAL;
    g_dragging = false;
    g_minimized = false;
    g_maximized = false;
    g_animating = true;
    g_anim_opening = true;
    g_anim_tick = scheduler_ticks();

    apps_redraw_active();
}

void apps_open_sysconfig(void) {
    if (!framebuffer_ready()) {
        return;
    }
    log_write("apps: open sysconfig");

    const u32 w = framebuffer_width();
    const u32 h = framebuffer_height();
    const u32 win_w = (w > 720) ? 560 : (w - 40);
    const u32 win_h = (h > 520) ? 360 : (h - 90);

    g_win.x = (w - win_w) / 2;
    g_win.y = (h - win_h) / 4;
    g_win.w = win_w;
    g_win.h = win_h;
    g_active = APP_SYSCONFIG;
    g_dragging = false;
    g_minimized = false;
    g_maximized = false;
    g_animating = true;
    g_anim_opening = true;
    g_anim_tick = scheduler_ticks();
    g_settings_page = SETTINGS_APPEARANCE;

    apps_redraw_active();
}

static void open_generic(active_app_t which, const char *logmsg, u32 win_w, u32 win_h, u32 div_y) {
    if (!framebuffer_ready()) {
        return;
    }
    const u32 w = framebuffer_width();
    const u32 h = framebuffer_height();
    if (win_w > w - 20) win_w = (w > 40) ? (w - 40) : w;
    if (win_h > h - 20) win_h = (h > 90) ? (h - 90) : h;
    g_win.x = (w - win_w) / 2;
    g_win.y = (h - win_h) / div_y;
    g_win.w = win_w;
    g_win.h = win_h;
    g_active = which;
    g_dragging = false;
    g_minimized = false;
    g_maximized = false;
    g_animating = true;
    g_anim_opening = true;
    g_anim_tick = scheduler_ticks();
    if (which == APP_CALC) {
        kmemset(g_calc_line, 0, sizeof(g_calc_line));
        g_calc_len = 0;
        g_calc_has_last = false;
        g_calc_last = 0;
    }
    if (which == APP_TYPING) {
        kmemset(g_type_buf, 0, sizeof(g_type_buf));
        g_type_len = 0;
    }
    if (which == APP_FILES) {
        if (g_fat_mounted) {
            kmemset(g_dir, 0, sizeof(g_dir));
            g_dir_count = fat32_list_root(g_dir, (sizeof(g_dir) / sizeof(g_dir[0])));
        } else {
            g_dir_count = 0;
        }
        g_dir_sel = -1;
    }
    if (logmsg) {
        log_write(logmsg);
    }
    apps_redraw_active();
}

void apps_open_calculator(void) {
    open_generic(APP_CALC, "apps: open calculator", 520, 320, 3);
}

void apps_open_clock(void) {
    open_generic(APP_CLOCK, "apps: open clock", 480, 260, 3);
}

void apps_open_files(void) {
    if (!g_fat_mounted) {
        g_fat_mounted = fat32_mount_primary();
        if (g_fat_mounted) {
            kmemset(g_dir, 0, sizeof(g_dir));
            g_dir_count = fat32_list_root(g_dir, (sizeof(g_dir) / sizeof(g_dir[0])));
            g_dir_sel = -1;
            notifications_post("FAT32 MOUNTED");
        } else {
            notifications_post("NO FAT32 DISK");
        }
    }
    open_generic(APP_FILES, "apps: open files", 720, 420, 4);
}

void apps_open_help(void) {
    open_generic(APP_HELP, "apps: open help", 620, 360, 4);
}

void apps_open_about(void) {
    open_generic(APP_ABOUT, "apps: open about", 520, 280, 3);
}

void apps_open_palette(void) {
    open_generic(APP_PALETTE, "apps: open palette", 680, 420, 4);
}

void apps_open_typing(void) {
    open_generic(APP_TYPING, "apps: open typing", 620, 360, 4);
}

void apps_open_trash(void) {
    open_generic(APP_TRASH, "apps: open trash", 520, 280, 3);
}

void apps_open_browser(void) {
    if (g_web_addr[0] == 0) {
        /* Default to example.com IP (HTTP). */
        const char *d = "93.184.216.34/";
        kmemset(g_web_addr, 0, sizeof(g_web_addr));
        for (usize i = 0; d[i] && i + 1 < sizeof(g_web_addr); i++) g_web_addr[i] = d[i];
        g_web_addr_len = 0;
        while (g_web_addr[g_web_addr_len]) g_web_addr_len++;
        g_web_addr_cursor = g_web_addr_len;
        kmemset(g_web_body, 0, sizeof(g_web_body));
        g_web_body_len = 0;
        g_web_scroll = 0;
    }
    if (g_web_router_name[0] == 0) {
        const char *n = "smileOS Router";
        kmemset(g_web_router_name, 0, sizeof(g_web_router_name));
        for (usize i = 0; n[i] && i + 1 < sizeof(g_web_router_name); i++) g_web_router_name[i] = n[i];
        g_web_router_name_len = 0;
        while (g_web_router_name[g_web_router_name_len]) g_web_router_name_len++;
        g_web_router_name_cursor = g_web_router_name_len;
        kmemset(g_web_router_pass, 0, sizeof(g_web_router_pass));
        g_web_router_pass_len = 0;
        g_web_router_pass_cursor = 0;
        g_web_focus = 0;
    }
    open_generic(APP_BROWSER, "apps: open web", 820, 520, 4);
}

static void buf_insert(char *buf, usize cap, usize *len, usize *cursor, char ch) {
    if (!buf || !len || !cursor) {
        return;
    }
    if (*len + 1 >= cap) {
        return;
    }
    if (*cursor > *len) {
        *cursor = *len;
    }
    for (usize i = *len; i > *cursor; i--) {
        buf[i] = buf[i - 1];
    }
    buf[*cursor] = ch;
    (*cursor)++;
    (*len)++;
    buf[*len] = 0;
}

static void buf_backspace(char *buf, usize *len, usize *cursor) {
    if (!buf || !len || !cursor || *len == 0 || *cursor == 0) {
        return;
    }
    for (usize i = *cursor - 1; i + 1 < *len; i++) {
        buf[i] = buf[i + 1];
    }
    (*cursor)--;
    (*len)--;
    buf[*len] = 0;
}

static void buf_move_left(usize *cursor) {
    if (cursor && *cursor) {
        (*cursor)--;
    }
}

static void buf_move_right(usize *cursor, usize len) {
    if (cursor && *cursor < len) {
        (*cursor)++;
    }
}

static void term_hist_push(const char *line) {
    if (!line) {
        return;
    }

    if (g_term_hist_count >= 32) {
        for (usize i = 1; i < 32; i++) {
            kmemcpy(g_term_hist[i - 1], g_term_hist[i], sizeof(g_term_hist[i]));
        }
        g_term_hist_count = 31;
    }

    kmemset(g_term_hist[g_term_hist_count], 0, sizeof(g_term_hist[g_term_hist_count]));
    for (usize i = 0; i < 127 && line[i] != 0; i++) {
        g_term_hist[g_term_hist_count][i] = line[i];
    }
    g_term_hist_count++;
}

static char upper_ascii(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    return c;
}

static bool streq_ci(const char *a, const char *b) {
    if (!a || !b) return false;
    usize i = 0;
    while (a[i] && b[i]) {
        if (upper_ascii(a[i]) != upper_ascii(b[i])) return false;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

static bool starts_with_ci(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    for (usize i = 0; prefix[i] != 0; i++) {
        if (s[i] == 0) return false;
        if (upper_ascii(s[i]) != upper_ascii(prefix[i])) return false;
    }
    return true;
}

static void term_exec_line(const char *line) {
    if (!line) return;

    /* Skip leading spaces. */
    usize p = 0;
    while (line[p] == ' ') p++;
    const char *s = &line[p];
    if (s[0] == 0) {
        return;
    }

    if (starts_with_ci(s, "HELP")) {
        term_hist_push("commands: help clear echo uptime ls cat open theme");
        term_hist_push("try: ls   cat README.TXT   open files   theme dark");
        return;
    }

    if (starts_with_ci(s, "CLEAR")) {
        for (usize i = 0; i < 32; i++) kmemset(g_term_hist[i], 0, sizeof(g_term_hist[i]));
        g_term_hist_count = 0;
        return;
    }

    if (starts_with_ci(s, "ECHO")) {
        usize q = 4;
        while (s[q] == ' ') q++;
        term_hist_push(&s[q]);
        return;
    }

    if (starts_with_ci(s, "UPTIME")) {
        const u64 ticks = scheduler_ticks();
        const u32 secs = (u32)(ticks / 50ULL);
        const u32 mm = (secs / 60U) % 100U;
        const u32 ss = secs % 60U;
        char buf[32];
        kmemset(buf, 0, sizeof(buf));
        buf[0] = 'u'; buf[1] = 'p'; buf[2] = 't'; buf[3] = 'i'; buf[4] = 'm'; buf[5] = 'e'; buf[6] = ':'; buf[7] = ' ';
        buf[8] = (char)('0' + (mm / 10U));
        buf[9] = (char)('0' + (mm % 10U));
        buf[10] = ':';
        buf[11] = (char)('0' + (ss / 10U));
        buf[12] = (char)('0' + (ss % 10U));
        term_hist_push(buf);
        return;
    }

    if (starts_with_ci(s, "LS")) {
        if (!g_fat_mounted) {
            term_hist_push("no fat32 disk (open files to mount)");
            return;
        }
        const usize max = (g_dir_count < 20) ? g_dir_count : 20;
        for (usize i = 0; i < max; i++) {
            term_hist_push(g_dir[i].name);
        }
        return;
    }

    if (starts_with_ci(s, "CAT")) {
        usize q = 3;
        while (s[q] == ' ') q++;
        if (s[q] == 0) {
            term_hist_push("usage: cat FILENAME.TXT");
            return;
        }
        if (!g_fat_mounted) {
            term_hist_push("no fat32 disk");
            return;
        }

        i32 found = -1;
        for (usize i = 0; i < g_dir_count; i++) {
            if (streq_ci(g_dir[i].name, &s[q])) {
                found = (i32)i;
                break;
            }
        }
        if (found < 0) {
            term_hist_push("file not found");
            return;
        }
        if (g_dir[found].is_dir) {
            term_hist_push("is a directory");
            return;
        }

        u8 tmp[768];
        u32 out_len = 0;
        if (!fat32_read_file(g_dir[found].first_cluster, g_dir[found].size, tmp, sizeof(tmp) - 1, &out_len)) {
            term_hist_push("read failed");
            return;
        }
        tmp[out_len] = 0;

        /* Print a small preview (up to 8 lines). */
        char lbuf[128];
        kmemset(lbuf, 0, sizeof(lbuf));
        usize li = 0;
        usize lines = 0;
        for (u32 i = 0; i < out_len && lines < 8; i++) {
            const char ch = (char)tmp[i];
            if (ch == '\r') continue;
            if (ch == '\n' || li + 1 >= sizeof(lbuf)) {
                lbuf[li] = 0;
                term_hist_push(lbuf);
                kmemset(lbuf, 0, sizeof(lbuf));
                li = 0;
                lines++;
                continue;
            }
            if ((unsigned char)ch >= 32 && (unsigned char)ch <= 126) {
                lbuf[li++] = ch;
            }
        }
        if (li && lines < 8) {
            lbuf[li] = 0;
            term_hist_push(lbuf);
        }
        return;
    }

    if (starts_with_ci(s, "OPEN")) {
        usize q = 4;
        while (s[q] == ' ') q++;
        if (s[q] == 0) {
            term_hist_push("usage: open notepad|files|settings|palette|clock|calc|typing|help|about");
            return;
        }

        if (starts_with_ci(&s[q], "NOTEPAD")) apps_open_notepad();
        else if (starts_with_ci(&s[q], "FILES")) apps_open_files();
        else if (starts_with_ci(&s[q], "SETTINGS") || starts_with_ci(&s[q], "SYSCONFIG")) apps_open_sysconfig();
        else if (starts_with_ci(&s[q], "PALETTE")) apps_open_palette();
        else if (starts_with_ci(&s[q], "CLOCK")) apps_open_clock();
        else if (starts_with_ci(&s[q], "CALC")) apps_open_calculator();
        else if (starts_with_ci(&s[q], "TYPING")) apps_open_typing();
        else if (starts_with_ci(&s[q], "HELP")) apps_open_help();
        else if (starts_with_ci(&s[q], "ABOUT")) apps_open_about();
        else term_hist_push("unknown app");
        return;
    }

    if (starts_with_ci(s, "THEME")) {
        usize q = 5;
        while (s[q] == ' ') q++;
        os_settings_t *st = settings_get();
        if (starts_with_ci(&s[q], "DARK")) {
            st->dark_theme = true;
            st->theme_mode = 1;
            desktop_redraw();
            term_hist_push("theme: dark");
        } else if (starts_with_ci(&s[q], "LIGHT")) {
            st->dark_theme = false;
            st->theme_mode = 0;
            desktop_redraw();
            term_hist_push("theme: light");
        } else {
            term_hist_push("usage: theme dark|light");
        }
        return;
    }

    term_hist_push("unknown command (type help)");
}

static void term_submit_line(void) {
    if (g_term_len == 0) {
        return;
    }

    /* Create "$ <cmd>" line in history. */
    {
        char cmdline[128];
        kmemset(cmdline, 0, sizeof(cmdline));
        cmdline[0] = '$';
        cmdline[1] = ' ';
        for (usize i = 0; i < g_term_len && i + 2 < sizeof(cmdline) - 1; i++) {
            cmdline[i + 2] = g_term_line[i];
        }
        term_hist_push(cmdline);
    }

    /* Parse and run. */
    g_term_line[g_term_len] = 0;
    term_exec_line(g_term_line);

    /* Clear input line. */
    kmemset(g_term_line, 0, sizeof(g_term_line));
    g_term_len = 0;
    g_term_cursor = 0;
}

void apps_handle_key(i32 keycode, bool pressed) {
    if (!pressed || g_active == APP_NONE) {
        return;
    }
    if (g_animating) {
        return;
    }

    if (keycode == KEY_ESC) {
        apps_close_active();
        return;
    }
    if (keycode == KEY_TAB) {
        /* Convenience: TAB minimizes. */
        apps_minimize_active();
        return;
    }

    if (g_active == APP_NOTEPAD) {
        if (keycode == KEY_BACKSPACE) {
            buf_backspace(g_note_buf, &g_note_len, &g_note_cursor);
        } else if (keycode == KEY_LEFT) {
            buf_move_left(&g_note_cursor);
        } else if (keycode == KEY_RIGHT) {
            buf_move_right(&g_note_cursor, g_note_len);
        } else if (keycode == '\n') {
            buf_insert(g_note_buf, sizeof(g_note_buf), &g_note_len, &g_note_cursor, '\n');
        } else if (keycode >= 32 && keycode <= 255) {
            buf_insert(g_note_buf, sizeof(g_note_buf), &g_note_len, &g_note_cursor, (char)keycode);
        }

        os_settings_t *s = settings_get();
        if (s->tutorial_enabled && !s->tutorial_completed && s->tutorial_step == 2) {
            if (keycode >= 32 && keycode <= 255) {
                s->tutorial_step = 3;
                log_write("tutorial: typed text");
            }
        }

        apps_redraw_active();
        return;
    }

    if (g_active == APP_TERMINAL) {
        if (keycode == KEY_BACKSPACE) {
            buf_backspace(g_term_line, &g_term_len, &g_term_cursor);
        } else if (keycode == KEY_LEFT) {
            buf_move_left(&g_term_cursor);
        } else if (keycode == KEY_RIGHT) {
            buf_move_right(&g_term_cursor, g_term_len);
        } else if (keycode == '\n') {
            term_submit_line();
        } else if (keycode >= 32 && keycode <= 255) {
            buf_insert(g_term_line, sizeof(g_term_line), &g_term_len, &g_term_cursor, (char)keycode);
        }
        apps_redraw_active();
        return;
    }

    if (g_active == APP_CALC) {
        if (keycode == KEY_BACKSPACE) {
            if (g_calc_len) {
                g_calc_len--;
                g_calc_line[g_calc_len] = 0;
            }
        } else if (keycode == '\n') {
            /* Parse "a op b" (ints) */
            i32 a = 0, b = 0;
            char op = 0;
            bool neg = false;
            usize i = 0;
            if (g_calc_line[i] == '-') { neg = true; i++; }
            for (; g_calc_line[i] >= '0' && g_calc_line[i] <= '9'; i++) {
                a = a * 10 + (g_calc_line[i] - '0');
            }
            if (neg) a = -a;
            while (g_calc_line[i] == ' ') i++;
            op = g_calc_line[i++];
            while (g_calc_line[i] == ' ') i++;
            neg = false;
            if (g_calc_line[i] == '-') { neg = true; i++; }
            for (; g_calc_line[i] >= '0' && g_calc_line[i] <= '9'; i++) {
                b = b * 10 + (g_calc_line[i] - '0');
            }
            if (neg) b = -b;

            i32 res = 0;
            bool ok = true;
            if (op == '+') res = a + b;
            else if (op == '-') res = a - b;
            else if (op == '*') res = a * b;
            else if (op == '/') { if (b == 0) ok = false; else res = a / b; }
            else ok = false;

            if (ok) {
                g_calc_last = res;
                g_calc_has_last = true;
                notifications_post("CALC OK");
            } else {
                notifications_post("CALC ERROR");
            }
            g_calc_len = 0;
            g_calc_line[0] = 0;
        } else if (keycode >= 32 && keycode <= 255) {
            const char ch = (char)keycode;
            if ((ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == ' ') {
                if (g_calc_len + 1 < sizeof(g_calc_line)) {
                    g_calc_line[g_calc_len++] = ch;
                    g_calc_line[g_calc_len] = 0;
                }
            }
        }
        desktop_redraw();
        apps_redraw_active();
        return;
    }

    if (g_active == APP_TYPING) {
        const char *target = "hello smileos";
        if (keycode == KEY_BACKSPACE) {
            if (g_type_len) {
                g_type_len--;
                g_type_buf[g_type_len] = 0;
            }
        } else if (keycode >= 32 && keycode <= 255) {
            if (g_type_len + 1 < sizeof(g_type_buf)) {
                g_type_buf[g_type_len++] = (char)keycode;
                g_type_buf[g_type_len] = 0;
            }
        }
        /* Completion check. */
        bool done = true;
        for (usize i = 0; target[i] != 0; i++) {
            if (g_type_buf[i] != target[i]) {
                done = false;
                break;
            }
        }
        if (done && g_type_len >= 1) {
            notifications_post("NICE! COMPLETED");
        }
        desktop_redraw();
        apps_redraw_active();
        return;
    }

    if (g_active == APP_BROWSER) {
        const net_state_t *ns = net_state();
        const bool connected = (ns && ns->up);
        if (keycode == KEY_BACKSPACE) {
            if (!connected) {
                if (g_web_focus == 0) buf_backspace(g_web_router_name, &g_web_router_name_len, &g_web_router_name_cursor);
                else if (g_web_focus == 1) buf_backspace(g_web_router_pass, &g_web_router_pass_len, &g_web_router_pass_cursor);
            } else {
                buf_backspace(g_web_addr, &g_web_addr_len, &g_web_addr_cursor);
            }
        } else if (keycode == KEY_LEFT) {
            if (!connected) {
                if (g_web_focus == 0) buf_move_left(&g_web_router_name_cursor);
                else if (g_web_focus == 1) buf_move_left(&g_web_router_pass_cursor);
            } else {
                buf_move_left(&g_web_addr_cursor);
            }
        } else if (keycode == KEY_RIGHT) {
            if (!connected) {
                if (g_web_focus == 0) buf_move_right(&g_web_router_name_cursor, g_web_router_name_len);
                else if (g_web_focus == 1) buf_move_right(&g_web_router_pass_cursor, g_web_router_pass_len);
            } else {
                buf_move_right(&g_web_addr_cursor, g_web_addr_len);
            }
        } else if (keycode == KEY_UP) {
            if (g_web_scroll > 0) g_web_scroll--;
        } else if (keycode == KEY_DOWN) {
            g_web_scroll++;
        } else if (keycode == KEY_TAB) {
            if (!connected) {
                g_web_focus = (u8)((g_web_focus + 1) % 2);
            }
        } else if (keycode == '\n') {
            if (!connected) {
                notifications_post("CONNECTING...");
                if (net_connect()) notifications_post("CONNECTED");
                else notifications_post("NO NETWORK");
                desktop_redraw();
                apps_redraw_active();
                return;
            }
            /* Parse "<ip>/<path>" */
            u32 ip = 0;
            char ipbuf[32];
            char pathbuf[96];
            kmemset(ipbuf, 0, sizeof(ipbuf));
            kmemset(pathbuf, 0, sizeof(pathbuf));
            usize ii = 0;
            usize pi = 0;
            for (usize i = 0; g_web_addr[i] && ii + 1 < sizeof(ipbuf); i++) {
                if (g_web_addr[i] == '/') {
                    pi = i;
                    break;
                }
                ipbuf[ii++] = g_web_addr[i];
            }
            if (pi == 0) {
                /* no path separator */
                pi = ii;
            }
            usize po = 0;
            for (usize i = pi; g_web_addr[i] && po + 1 < sizeof(pathbuf); i++) {
                pathbuf[po++] = g_web_addr[i];
            }
            if (pathbuf[0] == 0) {
                pathbuf[0] = '/';
                pathbuf[1] = 0;
            }
            if (!parse_ipv4(ipbuf, &ip)) {
                notifications_post("BAD IP");
            } else {
                notifications_post("FETCHING...");
                /* HTTPS is not implemented yet. */
                if (g_web_addr[0] == 'h' && g_web_addr[1] == 't' && g_web_addr[2] == 't' && g_web_addr[3] == 'p' && g_web_addr[4] == 's') {
                    notifications_post("HTTPS TODO");
                }
                char raw[4096];
                kmemset(raw, 0, sizeof(raw));
                const usize got = net_http_get(ip, pathbuf, raw, sizeof(raw));
                if (!got) {
                    notifications_post("HTTP FAILED");
                    kmemset(g_web_body, 0, sizeof(g_web_body));
                    g_web_body_len = 0;
                } else {
                    web_strip_to_body(raw, g_web_body, sizeof(g_web_body), &g_web_body_len);
                    g_web_scroll = 0;
                    notifications_post("DONE");
                }
            }
        } else if (keycode >= 32 && keycode <= 255) {
            if (!connected) {
                if (g_web_focus == 0) buf_insert(g_web_router_name, sizeof(g_web_router_name), &g_web_router_name_len, &g_web_router_name_cursor, (char)keycode);
                else if (g_web_focus == 1) buf_insert(g_web_router_pass, sizeof(g_web_router_pass), &g_web_router_pass_len, &g_web_router_pass_cursor, (char)keycode);
            } else {
                buf_insert(g_web_addr, sizeof(g_web_addr), &g_web_addr_len, &g_web_addr_cursor, (char)keycode);
            }
        }
        desktop_redraw();
        apps_redraw_active();
        return;
    }
}

cursor_state_t apps_cursor_state(i32 cursor_x, i32 cursor_y, bool left_down) {
    if (!framebuffer_ready() || g_active == APP_NONE) {
        return CURSOR_DEFAULT;
    }
    if (g_dragging && left_down) {
        return CURSOR_DRAG;
    }

    if (g_active == APP_NOTEPAD || g_active == APP_TERMINAL) {
        const i32 pad_x = (i32)g_win.x + 10;
        const i32 pad_y = (i32)g_win.y + 44;
        const i32 pad_w = (i32)g_win.w - 20;
        const i32 pad_h = (i32)g_win.h - 54;
        if (cursor_x >= pad_x && cursor_y >= pad_y &&
            cursor_x < pad_x + pad_w && cursor_y < pad_y + pad_h) {
            return CURSOR_TEXT;
        }
    }

    return CURSOR_DEFAULT;
}

bool apps_handle_mouse(const input_event_t *evt, i32 cursor_x, i32 cursor_y) {
    if (!evt || !framebuffer_ready() || g_active == APP_NONE) {
        return false;
    }
    if (g_animating) {
        return true;
    }

    if (evt->type == INPUT_EVENT_MOUSE_MOVE) {
        if (g_dragging && g_left_down) {
            app_window_t old = g_win;
            g_win.x = (u32)(cursor_x - g_drag_off_x);
            g_win.y = (u32)(cursor_y - g_drag_off_y);
            win_clamp_to_screen();
            /* Redraw only what we need: background behind old+new window. */
            const i32 m = 36; /* include shadow + a bit of blur */
            const i32 x0 = ((i32)old.x < (i32)g_win.x) ? (i32)old.x : (i32)g_win.x;
            const i32 y0 = ((i32)old.y < (i32)g_win.y) ? (i32)old.y : (i32)g_win.y;
            const i32 x1 = ((i32)old.x + (i32)old.w > (i32)g_win.x + (i32)g_win.w) ? (i32)old.x + (i32)old.w : (i32)g_win.x + (i32)g_win.w;
            const i32 y1 = ((i32)old.y + (i32)old.h > (i32)g_win.y + (i32)g_win.h) ? (i32)old.y + (i32)old.h : (i32)g_win.y + (i32)g_win.h;
            desktop_redraw_rect(x0 - m, y0 - m, (x1 - x0) + m * 2, (y1 - y0) + m * 2);
            apps_redraw_active();
            return true;
        }
        /* Don't steal hover from dock/desktop when the pointer is outside the window. */
        return pt_in_window(cursor_x, cursor_y);
    }

    if (evt->type != INPUT_EVENT_MOUSE_BUTTON) {
        return true;
    }

    const bool left = evt->a ? true : false;
    const i32 sh = (i32)framebuffer_height();
    /* Global UI layers (menu bar + dock) should remain clickable even if a window overlaps. */
    if (cursor_y < 28 || cursor_y > sh - 90) {
        if (!left) {
            g_left_down = false;
            g_dragging = false;
        }
        return false;
    }

    const bool in_win = pt_in_window(cursor_x, cursor_y);

    /* Release: always stop dragging, but only consume if inside the window. */
    if (!left) {
        g_left_down = false;
        g_dragging = false;
        return in_win;
    }

    /* Press */
    const bool rising = left && !g_left_down;
    if (!rising) {
        /* Defensive: ignore duplicate "down" events. */
        g_left_down = true;
        return in_win;
    }

    /* Click outside the window should fall through to the desktop (dock/icons). */
    if (!in_win && !g_dragging) {
        return false;
    }

    g_left_down = true;

    const i32 x = cursor_x;
    const i32 y = cursor_y;

    /* Window buttons hit test. */
    const i32 close_x = (i32)g_win.x + 14;
    const i32 min_x   = (i32)g_win.x + 34;
    const i32 max_x   = (i32)g_win.x + 54;
    const i32 btn_y   = (i32)g_win.y + 11;

    if (x >= close_x && x < close_x + 12 && y >= btn_y && y < btn_y + 12) {
        apps_close_active();
        return true;
    }

    if (x >= min_x && x < min_x + 12 && y >= btn_y && y < btn_y + 12) {
        apps_minimize_active();
        return true;
    }

    if (x >= max_x && x < max_x + 12 && y >= btn_y && y < btn_y + 12) {
        if (!g_maximized) {
            g_restore = g_win;
            g_win.x = 10;
            g_win.y = 10;
            g_win.w = framebuffer_width() - 20;
            g_win.h = framebuffer_height() - 66; /* leave taskbar */
            g_maximized = true;
        } else {
            g_win = g_restore;
            g_maximized = false;
        }
        desktop_redraw();
        apps_redraw_active();
        return true;
    }

    /* Start dragging when clicking title bar (excluding close button). */
    const i32 tb_x0 = (i32)g_win.x;
    const i32 tb_y0 = (i32)g_win.y;
    const i32 tb_x1 = (i32)g_win.x + (i32)g_win.w;
    const i32 tb_y1 = (i32)g_win.y + 34;
    if (x >= tb_x0 && x < tb_x1 && y >= tb_y0 && y < tb_y1) {
        const bool in_btns =
            (x >= close_x && x < close_x + 12 && y >= btn_y && y < btn_y + 12) ||
            (x >= min_x && x < min_x + 12 && y >= btn_y && y < btn_y + 12) ||
            (x >= max_x && x < max_x + 12 && y >= btn_y && y < btn_y + 12);
        if (!in_btns) {
            g_dragging = true;
            g_drag_off_x = x - (i32)g_win.x;
            g_drag_off_y = y - (i32)g_win.y;
            return true;
        }
    }

    /* System Settings hit tests (sidebar + controls). */
    if (g_active == APP_SYSCONFIG) {
        os_settings_t *s = settings_get();
        const i32 pad_x = (i32)g_win.x + 10;
        const i32 pad_y = (i32)g_win.y + 44;
        const i32 pad_w = (i32)g_win.w - 20;
        const i32 pad_h = (i32)g_win.h - 54;
        const i32 side_w = 170;

        /* Sidebar items */
        if (x >= pad_x && x < pad_x + side_w && y >= pad_y && y < pad_y + pad_h) {
            for (i32 i = 0; i < 6; i++) {
                const i32 iy = pad_y + 14 + i * 30;
                if (y >= iy - 6 && y < iy - 6 + 24) {
                    g_settings_page = (settings_page_t)i;
                    notifications_post("SETTINGS");
                    apps_redraw_active();
                    return true;
                }
            }
        }

        const i32 cx = pad_x + side_w + 14;
        const i32 cy = pad_y + 14;
        const i32 cw = pad_w - side_w - 28;

        if (g_settings_page == SETTINGS_APPEARANCE) {
            const i32 bw = 110;
            const i32 bh = 28;
            const i32 gap = 10;
            const i32 by = cy + 30;

            const i32 tx0 = cx;
            const i32 tx1 = cx + bw + gap;
            const i32 tx2 = cx + 2 * (bw + gap);
            const i32 tx3 = cx + 3 * (bw + gap);

            if (y >= by && y < by + bh) {
                u8 new_mode = s->theme_mode;
                if (x >= tx0 && x < tx0 + bw) new_mode = 0;
                else if (x >= tx1 && x < tx1 + bw) new_mode = 1;
                else if (x >= tx2 && x < tx2 + bw) new_mode = 2;
                else if (x >= tx3 && x < tx3 + bw) new_mode = 3;

                if (new_mode != s->theme_mode) {
                    s->theme_mode = new_mode;
                    s->dark_theme = (new_mode == 1 || new_mode == 3);
                    if (new_mode == 2 && s->accent_color == 0x3A7BFF) {
                        s->accent_color = 0x2DB7FF;
                    }
                    if (new_mode == 3 && s->accent_color == 0x3A7BFF) {
                        s->accent_color = 0x9AA4B2;
                    }
                    desktop_redraw();
                    notifications_post("THEME UPDATED");
                    apps_redraw_active();
                    return true;
                }
            }

            /* Accent swatches */
            const i32 sw_y = by + 78;
            if (y >= sw_y && y < sw_y + 18) {
                for (i32 i = 0; i < 10; i++) {
                    const i32 sx = cx + i * 26;
                    if (x >= sx && x < sx + 18) {
                        s->accent_color = palette_get((u16)(i * 997));
                        desktop_redraw();
                        notifications_post("ACCENT UPDATED");
                        apps_redraw_active();
                        return true;
                    }
                }
            }
        } else if (g_settings_page == SETTINGS_DOCK) {
            /* Auto-hide toggle */
            const i32 tx = cx + 160;
            const i32 ty = cy + 30;
            const i32 tw = 54;
            const i32 th = 22;
            if (x >= tx && x < tx + tw && y >= ty && y < ty + th) {
                s->dock_autohide = !s->dock_autohide;
                notifications_post("DOCK UPDATED");
                apps_redraw_active();
                return true;
            }
            /* DND toggle */
            const i32 ty2 = cy + 64;
            if (x >= tx && x < tx + tw && y >= ty2 && y < ty2 + th) {
                s->dnd = !s->dnd;
                notifications_post(s->dnd ? "DND ON" : "DND OFF");
                apps_redraw_active();
                return true;
            }
        } else if (g_settings_page == SETTINGS_INPUT) {
            /* Keep legacy toggles available here. */
            (void)cw;
        }

        return true;
    }

    if (g_active == APP_FILES) {
        /* Directory list click */
        const i32 pad_x = (i32)g_win.x + 10;
        const i32 pad_y = (i32)g_win.y + 44;
        const i32 pad_w = (i32)g_win.w - 20;
        const i32 pad_h = (i32)g_win.h - 54;
        const i32 list_x0 = pad_x + 10;
        const i32 list_x1 = pad_x + pad_w - 10;
        const i32 list_y0 = pad_y + 38;
        const i32 row_h = 16;

        if (x >= list_x0 && x < list_x1 && y >= list_y0 && y < pad_y + pad_h - 24) {
            const i32 idx = (y - list_y0) / row_h;
            if (idx >= 0 && (usize)idx < g_dir_count) {
                g_dir_sel = idx;
                notifications_post("SELECTED");

                /* Open small text files directly into Notepad (best-effort). */
                const fat32_dirent_t *de = &g_dir[idx];
                if (!de->is_dir && de->size > 0 && de->size < 1800) {
                    u8 tmp[2048];
                    u32 got = 0;
                    if (fat32_read_file(de->first_cluster, de->size, tmp, sizeof(tmp) - 1, &got)) {
                        tmp[got] = 0;
                        notepad_set_text((const char *)tmp, got);
                        apps_open_notepad();
                        notifications_post("OPENED IN NOTEPAD");
                        return true;
                    }
                    notifications_post("READ FAILED");
                }
                desktop_redraw();
                apps_redraw_active();
                return true;
            }
        }
    }

    return true;
}

void apps_redraw(void) {
    apps_redraw_active();
}

bool apps_needs_redraw(void) {
    if (g_active == APP_NONE) {
        return false;
    }
    if (g_animating) {
        const u64 dt = scheduler_ticks() - g_anim_tick;
        if (dt >= 14 && !g_anim_opening) {
            g_minimized = true;
            g_animating = false;
        }
        return true;
    }
    return false;
}
