#include "kernel.h"

void draw_init(void) {
    log_write("gfx: draw primitives initialized");
}

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

void draw_rect(u32 x, u32 y, u32 width, u32 height, u32 color) {
    framebuffer_fill_rect_fast(x, y, width, height, color);
}

void draw_rect_alpha(u32 x, u32 y, u32 width, u32 height, u32 color, u8 alpha) {
    if (!framebuffer_ready() || alpha == 0) {
        return;
    }
    if (alpha == 255) {
        draw_rect(x, y, width, height, color);
        return;
    }

    u32 max_x = x + width;
    u32 max_y = y + height;
    if (max_x > framebuffer_width()) {
        max_x = framebuffer_width();
    }
    if (max_y > framebuffer_height()) {
        max_y = framebuffer_height();
    }

    for (u32 py = y; py < max_y; py++) {
        for (u32 px = x; px < max_x; px++) {
            const u32 dst = framebuffer_get_pixel(px, py);
            framebuffer_put_pixel(px, py, blend_u32(dst, color, alpha));
        }
    }
}

void draw_shadow_rect(u32 x, u32 y, u32 width, u32 height) {
    if (!framebuffer_ready()) {
        return;
    }

    /* Simple 2-layer shadow. */
    const u32 s1 = 0x000000;
    draw_rect_alpha(x + 4, y + 6, width, height, s1, 70);
    draw_rect_alpha(x + 2, y + 3, width, height, s1, 95);
}

static void draw_circle_quadrant(u32 cx, u32 cy, i32 r, u32 color, bool right, bool down) {
    const i32 r2 = r * r;
    for (i32 dy = 0; dy <= r; dy++) {
        for (i32 dx = 0; dx <= r; dx++) {
            if (dx * dx + dy * dy > r2) {
                continue;
            }
            const i32 px = (i32)cx + (right ? dx : -dx);
            const i32 py = (i32)cy + (down ? dy : -dy);
            if (px < 0 || py < 0) continue;
            if ((u32)px >= framebuffer_width() || (u32)py >= framebuffer_height()) continue;
            framebuffer_put_pixel((u32)px, (u32)py, color);
        }
    }
}

static void draw_circle_quadrant_alpha(u32 cx, u32 cy, i32 r, u32 color, u8 alpha, bool right, bool down) {
    const i32 r2 = r * r;
    for (i32 dy = 0; dy <= r; dy++) {
        for (i32 dx = 0; dx <= r; dx++) {
            if (dx * dx + dy * dy > r2) {
                continue;
            }
            const i32 px = (i32)cx + (right ? dx : -dx);
            const i32 py = (i32)cy + (down ? dy : -dy);
            if (px < 0 || py < 0) continue;
            if ((u32)px >= framebuffer_width() || (u32)py >= framebuffer_height()) continue;
            const u32 dst = framebuffer_get_pixel((u32)px, (u32)py);
            framebuffer_put_pixel((u32)px, (u32)py, blend_u32(dst, color, alpha));
        }
    }
}

void draw_round_rect(u32 x, u32 y, u32 width, u32 height, u32 radius, u32 color) {
    if (!framebuffer_ready()) {
        return;
    }
    if (radius == 0 || width < 2 * radius || height < 2 * radius) {
        draw_rect(x, y, width, height, color);
        return;
    }

    const u32 r = radius;

    /* Center + sides */
    draw_rect(x + r, y, width - 2 * r, height, color);
    draw_rect(x, y + r, r, height - 2 * r, color);
    draw_rect(x + width - r, y + r, r, height - 2 * r, color);

    /* Corners */
    draw_circle_quadrant(x + r, y + r, (i32)r, color, false, false);
    draw_circle_quadrant(x + width - r - 1, y + r, (i32)r, color, true, false);
    draw_circle_quadrant(x + r, y + height - r - 1, (i32)r, color, false, true);
    draw_circle_quadrant(x + width - r - 1, y + height - r - 1, (i32)r, color, true, true);
}

void draw_round_rect_alpha(u32 x, u32 y, u32 width, u32 height, u32 radius, u32 color, u8 alpha) {
    if (!framebuffer_ready() || alpha == 0) {
        return;
    }
    if (alpha == 255) {
        draw_round_rect(x, y, width, height, radius, color);
        return;
    }
    if (radius == 0 || width < 2 * radius || height < 2 * radius) {
        draw_rect_alpha(x, y, width, height, color, alpha);
        return;
    }

    const u32 r = radius;

    draw_rect_alpha(x + r, y, width - 2 * r, height, color, alpha);
    draw_rect_alpha(x, y + r, r, height - 2 * r, color, alpha);
    draw_rect_alpha(x + width - r, y + r, r, height - 2 * r, color, alpha);

    draw_circle_quadrant_alpha(x + r, y + r, (i32)r, color, alpha, false, false);
    draw_circle_quadrant_alpha(x + width - r - 1, y + r, (i32)r, color, alpha, true, false);
    draw_circle_quadrant_alpha(x + r, y + height - r - 1, (i32)r, color, alpha, false, true);
    draw_circle_quadrant_alpha(x + width - r - 1, y + height - r - 1, (i32)r, color, alpha, true, true);
}

void draw_shadow_round_rect(u32 x, u32 y, u32 width, u32 height, u32 radius) {
    if (!framebuffer_ready()) {
        return;
    }
    const u32 s = 0x000000;
    draw_round_rect_alpha(x + 6, y + 10, width, height, radius, s, 55);
    draw_round_rect_alpha(x + 3, y + 6, width, height, radius, s, 80);
    draw_round_rect_alpha(x + 1, y + 3, width, height, radius, s, 95);
}

void draw_blur_rect(u32 x, u32 y, u32 width, u32 height, u32 step) {
    if (!framebuffer_ready() || width == 0 || height == 0) {
        return;
    }
    if (step == 0) {
        step = 1;
    }

    u32 max_x = x + width;
    u32 max_y = y + height;
    if (max_x > framebuffer_width()) max_x = framebuffer_width();
    if (max_y > framebuffer_height()) max_y = framebuffer_height();

    for (u32 py = y; py < max_y; py += step) {
        for (u32 px = x; px < max_x; px += step) {
            const u32 c0 = framebuffer_get_pixel(px, py);
            const u32 c1 = framebuffer_get_pixel((px > 0) ? (px - 1) : px, py);
            const u32 c2 = framebuffer_get_pixel((px + 1 < framebuffer_width()) ? (px + 1) : px, py);
            const u32 c3 = framebuffer_get_pixel(px, (py > 0) ? (py - 1) : py);
            const u32 c4 = framebuffer_get_pixel(px, (py + 1 < framebuffer_height()) ? (py + 1) : py);

            const u32 r = (((c0 >> 16) & 0xFF) + ((c1 >> 16) & 0xFF) + ((c2 >> 16) & 0xFF) + ((c3 >> 16) & 0xFF) + ((c4 >> 16) & 0xFF)) / 5U;
            const u32 g = (((c0 >> 8) & 0xFF) + ((c1 >> 8) & 0xFF) + ((c2 >> 8) & 0xFF) + ((c3 >> 8) & 0xFF) + ((c4 >> 8) & 0xFF)) / 5U;
            const u32 b = ((c0 & 0xFF) + (c1 & 0xFF) + (c2 & 0xFF) + (c3 & 0xFF) + (c4 & 0xFF)) / 5U;
            const u32 blur = (r << 16) | (g << 8) | b;

            for (u32 by = 0; by < step; by++) {
                for (u32 bx = 0; bx < step; bx++) {
                    const u32 fx = px + bx;
                    const u32 fy = py + by;
                    if (fx >= max_x || fy >= max_y) continue;
                    framebuffer_put_pixel(fx, fy, blur);
                }
            }
        }
    }
}
