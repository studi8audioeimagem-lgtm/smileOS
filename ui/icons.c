#include "kernel.h"
#include "../gfx/colors.h"
#include "types.h"
#include "icons.h"

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

static void put_px(i32 x, i32 y, u32 c) {
    if (x < 0 || y < 0) return;
    if (!framebuffer_ready()) return;
    if ((u32)x >= framebuffer_width() || (u32)y >= framebuffer_height()) return;
    framebuffer_put_pixel((u32)x, (u32)y, c);
}

static void fill_circle(i32 cx, i32 cy, i32 r, u32 c) {
    const i32 r2 = r * r;
    for (i32 y = -r; y <= r; y++) {
        for (i32 x = -r; x <= r; x++) {
            if (x * x + y * y <= r2) {
                put_px(cx + x, cy + y, c);
            }
        }
    }
}

static void stroke_circle(i32 cx, i32 cy, i32 r, u32 c) {
    const i32 r2 = r * r;
    const i32 in = r - 1;
    const i32 in2 = in * in;
    for (i32 y = -r; y <= r; y++) {
        for (i32 x = -r; x <= r; x++) {
            const i32 d2 = x * x + y * y;
            if (d2 <= r2 && d2 >= in2) {
                put_px(cx + x, cy + y, c);
            }
        }
    }
}

static void base_icon(i32 x, i32 y, i32 sz, u32 top, u32 bot, bool active) {
    const u32 r = (sz >= 44) ? 12 : 10;
    const u32 border = active ? settings_accent_color() : COLOR_PANEL_BORDER;
    draw_shadow_round_rect((u32)x, (u32)y, (u32)sz, (u32)sz, r);
    draw_round_rect((u32)x, (u32)y, (u32)sz, (u32)sz, r, border);

    /* Gradient fill */
    for (i32 row = 0; row < sz; row++) {
        const u32 t = (u32)(row * 255U) / (u32)(sz ? sz : 1);
        const u32 c = lerp_rgb(top, bot, t);
        draw_round_rect_alpha((u32)x + 1, (u32)y + 1 + (u32)row, (u32)sz - 2, 1, r - 1, c, 255);
    }
    /* Gloss */
    draw_round_rect_alpha((u32)x + 2, (u32)y + 2, (u32)sz - 4, (u32)(sz / 2), r - 2, COLOR_WHITE, 26);
}

static void sym_notepad(i32 x, i32 y, i32 sz) {
    const i32 pad = sz / 5;
    const i32 rx = x + pad;
    const i32 ry = y + pad;
    const i32 rw = sz - 2 * pad;
    const i32 rh = sz - 2 * pad;
    draw_round_rect((u32)rx, (u32)ry, (u32)rw, (u32)rh, 6, 0xF7FAFF);
    draw_round_rect((u32)rx, (u32)ry, (u32)rw, (u32)rh, 6, 0xD3DCEB);
    for (i32 i = 0; i < 4; i++) {
        draw_rect((u32)(rx + 6), (u32)(ry + 10 + i * 8), (u32)(rw - 12), 1, 0xA6B3C8);
    }
    /* Binding */
    draw_rect((u32)(rx + 6), (u32)(ry + 5), (u32)(rw - 12), 3, 0x3A7BFF);
}

static void sym_terminal(i32 x, i32 y, i32 sz) {
    const i32 pad = sz / 5;
    const i32 rx = x + pad;
    const i32 ry = y + pad;
    const i32 rw = sz - 2 * pad;
    const i32 rh = sz - 2 * pad;
    draw_round_rect((u32)rx, (u32)ry, (u32)rw, (u32)rh, 6, 0x0B0E14);
    draw_round_rect((u32)rx, (u32)ry, (u32)rw, (u32)rh, 6, 0x1C2533);
    /* Prompt: >_ */
    draw_rect((u32)(rx + 8), (u32)(ry + rh / 2 - 3), 10, 2, 0x28C840);
    draw_rect((u32)(rx + 16), (u32)(ry + rh / 2 - 1), 2, 2, 0x28C840);
    draw_rect((u32)(rx + 22), (u32)(ry + rh / 2 + 2), 10, 2, 0xEAF2FF);
}

static void sym_settings(i32 x, i32 y, i32 sz) {
    const i32 cx = x + sz / 2;
    const i32 cy = y + sz / 2;
    const i32 r = sz / 5;
    fill_circle(cx, cy, r + 5, 0xDDE5F2);
    stroke_circle(cx, cy, r + 5, 0x9AA4B2);
    fill_circle(cx, cy, r, 0x9AA4B2);
    for (i32 i = 0; i < 6; i++) {
        const i32 dx = (i == 0) ? 0 : (i == 3) ? 0 : (i == 1 || i == 2) ? 7 : -7;
        const i32 dy = (i == 1) ? -7 : (i == 4) ? 7 : (i == 0) ? -10 : (i == 3) ? 10 : (i == 2) ? -3 : 3;
        fill_circle(cx + dx, cy + dy, 2, 0x9AA4B2);
    }
}

static void sym_folder(i32 x, i32 y, i32 sz) {
    const i32 pad = sz / 6;
    const i32 rx = x + pad;
    const i32 ry = y + pad + 4;
    const i32 rw = sz - 2 * pad;
    const i32 rh = sz - 2 * pad - 6;
    draw_round_rect((u32)rx, (u32)ry, (u32)rw, (u32)rh, 6, 0xF4C95D);
    draw_round_rect((u32)rx, (u32)ry, (u32)rw, (u32)rh, 6, 0xC89A2C);
    draw_round_rect((u32)(rx + 4), (u32)(ry - 6), (u32)(rw / 2), 10, 5, 0xF7DA7E);
}

static void sym_calculator(i32 x, i32 y, i32 sz) {
    const i32 pad = sz / 6;
    const i32 rx = x + pad;
    const i32 ry = y + pad;
    const i32 rw = sz - 2 * pad;
    const i32 rh = sz - 2 * pad;
    draw_round_rect((u32)rx, (u32)ry, (u32)rw, (u32)rh, 8, 0x101216);
    draw_round_rect((u32)rx, (u32)ry, (u32)rw, (u32)rh, 8, 0x2E3642);
    draw_round_rect((u32)(rx + 6), (u32)(ry + 6), (u32)(rw - 12), 10, 5, 0xDDE5F2);
    for (i32 gy = 0; gy < 3; gy++) {
        for (i32 gx = 0; gx < 3; gx++) {
            draw_round_rect((u32)(rx + 6 + gx * 10), (u32)(ry + 22 + gy * 10), 8, 8, 3, 0x3A7BFF);
        }
    }
}

static void sym_clock(i32 x, i32 y, i32 sz) {
    const i32 cx = x + sz / 2;
    const i32 cy = y + sz / 2;
    const i32 r = sz / 4;
    fill_circle(cx, cy, r + 6, 0xF7FAFF);
    stroke_circle(cx, cy, r + 6, 0x9AA4B2);
    /* hands */
    for (i32 i = 0; i < r; i++) put_px(cx, cy - i, 0x3A7BFF);
    for (i32 i = 0; i < r - 2; i++) put_px(cx + i, cy, 0x3A7BFF);
    fill_circle(cx, cy, 2, 0x3A7BFF);
}

static void sym_palette(i32 x, i32 y, i32 sz) {
    const i32 cx = x + sz / 2;
    const i32 cy = y + sz / 2;
    fill_circle(cx - 2, cy + 2, sz / 4, 0xF7D7B6);
    stroke_circle(cx - 2, cy + 2, sz / 4, 0xB98A63);
    fill_circle(cx + 6, cy + 6, sz / 10, 0xE9EEF5); /* thumb hole */
    fill_circle(cx - 10, cy - 4, 2, 0xE74C3C);
    fill_circle(cx - 4, cy - 10, 2, 0xF39C12);
    fill_circle(cx + 4, cy - 8, 2, 0x2ECC71);
    fill_circle(cx + 10, cy - 2, 2, 0x3A7BFF);
}

static void sym_typing(i32 x, i32 y, i32 sz) {
    const i32 pad = sz / 6;
    const i32 rx = x + pad;
    const i32 ry = y + pad + 6;
    const i32 rw = sz - 2 * pad;
    const i32 rh = sz - 2 * pad - 10;
    draw_round_rect((u32)rx, (u32)ry, (u32)rw, (u32)rh, 6, 0xE9EEF5);
    draw_round_rect((u32)rx, (u32)ry, (u32)rw, (u32)rh, 6, 0x9AA4B2);
    for (i32 i = 0; i < 3; i++) {
        draw_rect((u32)(rx + 6), (u32)(ry + 6 + i * 8), (u32)(rw - 12), 1, 0x9AA4B2);
    }
}

static void sym_help(i32 x, i32 y, i32 sz) {
    const i32 cx = x + sz / 2;
    const i32 cy = y + sz / 2;
    fill_circle(cx, cy, sz / 4, 0x2DB7FF);
    stroke_circle(cx, cy, sz / 4, 0x0A6EA6);
    text_draw_scaled((u32)(cx - 6), (u32)(cy - 10), "?", COLOR_WHITE, 2);
}

static void sym_about(i32 x, i32 y, i32 sz) {
    const i32 cx = x + sz / 2;
    const i32 cy = y + sz / 2;
    fill_circle(cx, cy, sz / 4, 0x9AA4B2);
    stroke_circle(cx, cy, sz / 4, 0x5A6472);
    text_draw_scaled((u32)(cx - 4), (u32)(cy - 10), "i", COLOR_WHITE, 2);
}

static void sym_trash(i32 x, i32 y, i32 sz) {
    const i32 pad = sz / 5;
    const i32 rx = x + pad;
    const i32 ry = y + pad + 4;
    const i32 rw = sz - 2 * pad;
    const i32 rh = sz - 2 * pad - 6;
    draw_round_rect((u32)rx, (u32)ry, (u32)rw, (u32)rh, 6, 0xE9EEF5);
    draw_round_rect((u32)rx, (u32)ry, (u32)rw, (u32)rh, 6, 0x9AA4B2);
    draw_round_rect((u32)(rx + 4), (u32)(ry - 6), (u32)(rw - 8), 8, 4, 0x9AA4B2);
    draw_rect((u32)(rx + 8), (u32)(ry + 8), 1, (u32)(rh - 16), 0x9AA4B2);
    draw_rect((u32)(rx + 14), (u32)(ry + 8), 1, (u32)(rh - 16), 0x9AA4B2);
    draw_rect((u32)(rx + 20), (u32)(ry + 8), 1, (u32)(rh - 16), 0x9AA4B2);
}

static void sym_photos(i32 x, i32 y, i32 sz) {
    const i32 pad = sz / 6;
    const i32 rx = x + pad;
    const i32 ry = y + pad + 2;
    const i32 rw = sz - 2 * pad;
    const i32 rh = sz - 2 * pad - 4;
    draw_round_rect((u32)rx, (u32)ry, (u32)rw, (u32)rh, 7, 0xF7FAFF);
    draw_round_rect((u32)rx, (u32)ry, (u32)rw, (u32)rh, 7, 0x9AA4B2);
    /* Mountains + sun */
    draw_rect((u32)(rx + 6), (u32)(ry + rh - 14), (u32)(rw - 12), 1, 0x9AA4B2);
    fill_circle(rx + rw - 12, ry + 12, 4, 0xF39C12);
    draw_round_rect_alpha((u32)(rx + 8), (u32)(ry + rh - 18), 18, 10, 5, 0x2DB7FF, 220);
    draw_round_rect_alpha((u32)(rx + 20), (u32)(ry + rh - 22), 18, 14, 6, 0x2ECC71, 220);
}

static void sym_music(i32 x, i32 y, i32 sz) {
    const i32 cx = x + sz / 2;
    const i32 cy = y + sz / 2;
    /* Note stem */
    for (i32 i = 0; i < sz / 4; i++) {
        put_px(cx + 6, cy - 10 + i, COLOR_WHITE);
        put_px(cx + 7, cy - 10 + i, COLOR_WHITE);
    }
    /* Note head */
    fill_circle(cx - 2, cy + 8, 5, COLOR_WHITE);
    /* Flag */
    draw_round_rect_alpha((u32)(cx + 7), (u32)(cy - 10), 10, 6, 3, COLOR_WHITE, 220);
}

static void sym_browser(i32 x, i32 y, i32 sz) {
    const i32 cx = x + sz / 2;
    const i32 cy = y + sz / 2;
    const i32 r = sz / 4;
    stroke_circle(cx, cy, r + 6, 0xE9EEF5);
    /* Latitude/longitude lines */
    for (i32 dx = -r; dx <= r; dx++) {
        if (dx % 4 == 0) {
            put_px(cx + dx, cy, 0xE9EEF5);
        }
    }
    for (i32 dy = -r; dy <= r; dy++) {
        if (dy % 4 == 0) {
            put_px(cx, cy + dy, 0xE9EEF5);
        }
    }
}

static void sym_wifi(i32 x, i32 y, i32 sz) {
    const i32 cx = x + sz / 2;
    const i32 cy = y + sz / 2 + 4;
    stroke_circle(cx, cy, 10, COLOR_WHITE);
    stroke_circle(cx, cy, 7, COLOR_WHITE);
    stroke_circle(cx, cy, 4, COLOR_WHITE);
    fill_circle(cx, cy, 2, COLOR_WHITE);
}

static void sym_volume(i32 x, i32 y, i32 sz) {
    const i32 cx = x + sz / 2 - 2;
    const i32 cy = y + sz / 2;
    draw_round_rect_alpha((u32)(cx - 10), (u32)(cy - 5), 8, 10, 3, COLOR_WHITE, 220);
    draw_round_rect_alpha((u32)(cx - 3), (u32)(cy - 8), 6, 16, 3, COLOR_WHITE, 220);
    /* waves */
    stroke_circle(cx + 8, cy, 6, COLOR_WHITE);
}

static void sym_battery(i32 x, i32 y, i32 sz) {
    const i32 rx = x + sz / 4;
    const i32 ry = y + sz / 2 - 5;
    const i32 rw = sz / 2;
    const i32 rh = 10;
    draw_round_rect((u32)rx, (u32)ry, (u32)rw, (u32)rh, 4, COLOR_WHITE);
    draw_round_rect_alpha((u32)(rx + 1), (u32)(ry + 1), (u32)(rw - 2), (u32)(rh - 2), 3, 0x2ECC71, 220);
    draw_rect((u32)(rx + rw), (u32)(ry + 3), 2, 4, COLOR_WHITE);
}

static void sym_bell(i32 x, i32 y, i32 sz) {
    const i32 cx = x + sz / 2;
    const i32 cy = y + sz / 2 - 2;
    stroke_circle(cx, cy, 8, COLOR_WHITE);
    draw_rect((u32)(cx - 8), (u32)(cy + 2), 16, 8, COLOR_WHITE);
    fill_circle(cx, cy + 10, 2, COLOR_WHITE);
}

static void sym_search(i32 x, i32 y, i32 sz) {
    const i32 cx = x + sz / 2 - 2;
    const i32 cy = y + sz / 2 - 2;
    stroke_circle(cx, cy, 7, COLOR_WHITE);
    draw_rect((u32)(cx + 5), (u32)(cy + 5), 6, 2, COLOR_WHITE);
}

void icon_draw(icon_id_t id, i32 x, i32 y, i32 size, bool active) {
    if (!framebuffer_ready() || size < 24) {
        return;
    }

    u32 top = 0x3A7BFF;
    u32 bot = 0x1D3E6A;
    switch (id) {
        case ICON_NOTEPAD:     top = 0x3A7BFF; bot = 0x1D3E6A; break;
        case ICON_TERMINAL:    top = 0x202634; bot = 0x0B0E14; break;
        case ICON_SETTINGS:    top = 0x8A94A6; bot = 0x3B4456; break;
        case ICON_FILES:       top = 0xF8D77E; bot = 0xD39B2A; break;
        case ICON_CALCULATOR:  top = 0x2E3642; bot = 0x101216; break;
        case ICON_CLOCK:       top = 0x2DB7FF; bot = 0x0A6EA6; break;
        case ICON_PALETTE:     top = 0xFF9B7A; bot = 0x9C4E3C; break;
        case ICON_TYPING:      top = 0x9AA4B2; bot = 0x3B4456; break;
        case ICON_HELP:        top = 0x2DB7FF; bot = 0x0A6EA6; break;
        case ICON_ABOUT:       top = 0x9AA4B2; bot = 0x5A6472; break;
        case ICON_PHOTOS:      top = 0x8FC6FF; bot = 0x2C7CFF; break;
        case ICON_MUSIC:       top = 0xFF7AB6; bot = 0x8C2D5F; break;
        case ICON_BROWSER:     top = 0x2DB7FF; bot = 0x0A6EA6; break;
        case ICON_WIFI:        top = 0x3A7BFF; bot = 0x1D3E6A; break;
        case ICON_VOLUME:      top = 0x9AA4B2; bot = 0x3B4456; break;
        case ICON_BATTERY:     top = 0x2ECC71; bot = 0x1E8A56; break;
        case ICON_BELL:        top = 0xF39C12; bot = 0x9C5F12; break;
        case ICON_SEARCH:      top = 0x3A7BFF; bot = 0x1D3E6A; break;
        case ICON_TRASH:       top = 0x5A6472; bot = 0x222833; break;
    }

    base_icon(x, y, size, top, bot, active);

    switch (id) {
        case ICON_NOTEPAD: sym_notepad(x, y, size); break;
        case ICON_TERMINAL: sym_terminal(x, y, size); break;
        case ICON_SETTINGS: sym_settings(x, y, size); break;
        case ICON_FILES: sym_folder(x, y, size); break;
        case ICON_CALCULATOR: sym_calculator(x, y, size); break;
        case ICON_CLOCK: sym_clock(x, y, size); break;
        case ICON_PALETTE: sym_palette(x, y, size); break;
        case ICON_TYPING: sym_typing(x, y, size); break;
        case ICON_HELP: sym_help(x, y, size); break;
        case ICON_ABOUT: sym_about(x, y, size); break;
        case ICON_PHOTOS: sym_photos(x, y, size); break;
        case ICON_MUSIC: sym_music(x, y, size); break;
        case ICON_BROWSER: sym_browser(x, y, size); break;
        case ICON_WIFI: sym_wifi(x, y, size); break;
        case ICON_VOLUME: sym_volume(x, y, size); break;
        case ICON_BATTERY: sym_battery(x, y, size); break;
        case ICON_BELL: sym_bell(x, y, size); break;
        case ICON_SEARCH: sym_search(x, y, size); break;
        case ICON_TRASH: sym_trash(x, y, size); break;
    }
}
