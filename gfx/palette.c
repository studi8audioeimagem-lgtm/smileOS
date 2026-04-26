#include "kernel.h"
#include "types.h"

enum { PALETTE_SIZE = 10000 };

static u32 g_palette[PALETTE_SIZE];
static bool g_palette_ready;

static u32 pack_rgb(u32 r, u32 g, u32 b) {
    return ((r & 0xFFU) << 16) | ((g & 0xFFU) << 8) | (b & 0xFFU);
}

static u32 hsv_to_rgb(u32 h, u32 s, u32 v) {
    const u32 region = h / 60U;
    const u32 rem = (h - region * 60U) * 255U / 60U;
    const u32 p = (v * (255U - s)) / 255U;
    const u32 q = (v * (255U - (s * rem) / 255U)) / 255U;
    const u32 t = (v * (255U - (s * (255U - rem)) / 255U)) / 255U;

    switch (region) {
        case 0: return pack_rgb(v, t, p);
        case 1: return pack_rgb(q, v, p);
        case 2: return pack_rgb(p, v, t);
        case 3: return pack_rgb(p, q, v);
        case 4: return pack_rgb(t, p, v);
        default: return pack_rgb(v, p, q);
    }
}

static void palette_build(void) {
    /* 100 hues x 10 sats x 10 vals = 10,000 colors. */
    u32 idx = 0;
    for (u32 hi = 0; hi < 100; hi++) {
        const u32 h = (hi * 360U) / 100U;
        for (u32 si = 0; si < 10; si++) {
            const u32 s = 40U + (si * 215U) / 9U;
            for (u32 vi = 0; vi < 10; vi++) {
                const u32 v = 40U + (vi * 215U) / 9U;
                g_palette[idx++] = hsv_to_rgb(h, s, v);
            }
        }
    }
    g_palette_ready = true;
}

u16 palette_size(void) {
    return PALETTE_SIZE;
}

u32 palette_get(u16 idx) {
    if (!g_palette_ready) {
        palette_build();
    }
    return g_palette[idx % PALETTE_SIZE];
}
