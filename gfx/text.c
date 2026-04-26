#include "kernel.h"
#include "types.h"
#include "font_atlas.h"

static const font_face_t *g_face = 0;

static inline u32 blend_u32(u32 dst, u32 src, u8 a) {
    const u32 inv = 255U - (u32)a;
    const u32 sr = (src >> 16) & 0xFF;
    const u32 sg = (src >> 8) & 0xFF;
    const u32 sb = src & 0xFF;
    const u32 dr = (dst >> 16) & 0xFF;
    const u32 dg = (dst >> 8) & 0xFF;
    const u32 db = dst & 0xFF;
    const u32 r = (sr * a + dr * inv) / 255U;
    const u32 g = (sg * a + dg * inv) / 255U;
    const u32 b = (sb * a + db * inv) / 255U;
    return (r << 16) | (g << 8) | b;
}

u32 text_line_height(void) {
    const font_face_t *f = g_face ? g_face : &g_font_face_ui;
    return f->line_height ? f->line_height : 9;
}

u32 text_cell_advance(void) {
    const font_face_t *f = g_face ? g_face : &g_font_face_ui;
    return f->cell_advance ? f->cell_advance : 6;
}

u32 text_ascent(void) {
    const font_face_t *f = g_face ? g_face : &g_font_face_ui;
    return f->ascent ? f->ascent : 7;
}

void text_set_face(const void *face) {
    g_face = (const font_face_t *)face;
}

const void *text_get_face(void) {
    return (const void *)g_face;
}

static void draw_glyph_alpha(i32 x, i32 y, const font_glyph_t *g, u32 color, u32 scale) {
    if (!framebuffer_ready() || !g || g->w == 0 || g->h == 0) {
        return;
    }
    const font_face_t *f = g_face ? g_face : &g_font_face_ui;
    if (!f->atlas_alpha || f->atlas_w == 0 || f->atlas_h == 0) {
        return;
    }
    if (scale == 0) scale = 1;

    const u32 aw = f->atlas_w;
    const u32 ah = f->atlas_h;
    if (g->x + g->w > aw || g->y + g->h > ah) {
        return;
    }

    for (u32 gy = 0; gy < g->h; gy++) {
        for (u32 gx = 0; gx < g->w; gx++) {
            const u8 a = f->atlas_alpha[(usize)(g->y + gy) * (usize)aw + (usize)(g->x + gx)];
            if (!a) continue;

            const i32 px0 = x + (i32)gx * (i32)scale;
            const i32 py0 = y + (i32)gy * (i32)scale;

            for (u32 sy = 0; sy < scale; sy++) {
                const i32 py = py0 + (i32)sy;
                if (py < 0) continue;
                if ((u32)py >= framebuffer_height()) continue;
                for (u32 sx = 0; sx < scale; sx++) {
                    const i32 px = px0 + (i32)sx;
                    if (px < 0) continue;
                    if ((u32)px >= framebuffer_width()) continue;
                    const u32 dst = framebuffer_get_pixel((u32)px, (u32)py);
                    framebuffer_put_pixel((u32)px, (u32)py, blend_u32(dst, color, a));
                }
            }
        }
    }
}

void text_draw_scaled(u32 x, u32 y, const char *s, u32 color, u32 scale) {
    if (!framebuffer_ready() || !s) {
        return;
    }
    if (scale == 0) scale = 1;

    i32 pen_x = (i32)x;
    i32 pen_y = (i32)y;
    const u32 line_h = text_line_height() * scale;

    for (usize i = 0; s[i] != 0; i++) {
        const unsigned char ch = (unsigned char)s[i];
        if (ch == '\n') {
            pen_x = (i32)x;
            pen_y += (i32)line_h;
            continue;
        }

        const font_face_t *f = g_face ? g_face : &g_font_face_ui;
        const font_glyph_t *g = &f->glyphs[ch];
        const u32 adv = g->xadvance ? g->xadvance : text_cell_advance();

        if (g->w && g->h) {
            const i32 gx = pen_x + (i32)g->xoff * (i32)scale;
            /* g->yoff is already relative to the top of the line (baker stored ascent-top). */
            const i32 gy = pen_y + (i32)g->yoff * (i32)scale;
            draw_glyph_alpha(gx, gy, g, color, scale);
        }

        pen_x += (i32)(adv * scale);
    }
}

void text_draw(u32 x, u32 y, const char *s, u32 color) {
    text_draw_scaled(x, y, s, color, 1);
}
