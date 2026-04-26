#include "kernel.h"
#include "types.h"
#include "string.h"

extern u64 g_multiboot2_info_ptr;

typedef struct {
    u32 total_size;
    u32 reserved;
} mb2_info_header_t;

typedef struct {
    u32 type;
    u32 size;
} mb2_tag_t;

typedef struct {
    u32 type;
    u32 size;
    u64 framebuffer_addr;
    u32 framebuffer_pitch;
    u32 framebuffer_width;
    u32 framebuffer_height;
    u8 framebuffer_bpp;
    u8 framebuffer_type;
    u16 reserved;
} mb2_framebuffer_tag_t;

static u8 *g_fb_front;
static u8 *g_fb_draw;
static u8 *g_fb_back;
static u32 g_fb_pitch;
static u32 g_fb_width;
static u32 g_fb_height;
static bool g_fb_ready;
static const usize g_boot_identity_map_limit = 0x100000000ULL; /* 4 GiB */

/* Dirty tracking (in draw buffer coordinates). */
static bool g_damage_any;
static bool g_damage_full;
static u32 g_damage_x0;
static u32 g_damage_y0;
static u32 g_damage_x1;
static u32 g_damage_y1;

static inline u32 align_up_8(u32 n) {
    return (n + 7U) & ~7U;
}

static const mb2_framebuffer_tag_t *find_fb_tag(void) {
    if (g_multiboot2_info_ptr == 0) {
        return 0;
    }

    const mb2_info_header_t *hdr = (const mb2_info_header_t *)(usize)g_multiboot2_info_ptr;
    const u8 *end = ((const u8 *)hdr) + hdr->total_size;
    const mb2_tag_t *tag = (const mb2_tag_t *)(((const u8 *)hdr) + 8);

    while ((const u8 *)tag < end && tag->type != 0) {
        if (tag->type == 8) {
            return (const mb2_framebuffer_tag_t *)tag;
        }
        tag = (const mb2_tag_t *)(((const u8 *)tag) + align_up_8(tag->size));
    }

    return 0;
}

bool framebuffer_ready(void) {
    return g_fb_ready;
}

u32 framebuffer_width(void) {
    return g_fb_width;
}

u32 framebuffer_height(void) {
    return g_fb_height;
}

u32 framebuffer_pitch(void) {
    return g_fb_pitch;
}

u32 framebuffer_get_pixel(u32 x, u32 y) {
    if (!g_fb_ready || x >= g_fb_width || y >= g_fb_height) {
        return 0;
    }

    const u32 *row = (const u32 *)(const void *)(g_fb_draw + ((usize)y * g_fb_pitch));
    return row[x];
}

void framebuffer_put_pixel(u32 x, u32 y, u32 color) {
    if (!g_fb_ready || x >= g_fb_width || y >= g_fb_height) {
        return;
    }

    u32 *row = (u32 *)(void *)(g_fb_draw + ((usize)y * g_fb_pitch));
    row[x] = color;
}

void framebuffer_fill(u32 color) {
    if (!g_fb_ready) {
        return;
    }

    framebuffer_fill_rect_fast(0, 0, g_fb_width, g_fb_height, color);
}

void framebuffer_fill_rect_fast(u32 x, u32 y, u32 width, u32 height, u32 color) {
    if (!g_fb_ready || !g_fb_draw) {
        return;
    }
    if (width == 0 || height == 0) {
        return;
    }
    if (x >= g_fb_width || y >= g_fb_height) {
        return;
    }

    u32 w = width;
    u32 h = height;
    if (x + w > g_fb_width) w = g_fb_width - x;
    if (y + h > g_fb_height) h = g_fb_height - y;

    for (u32 yy = 0; yy < h; yy++) {
        u32 *row = (u32 *)(void *)(g_fb_draw + ((usize)(y + yy) * (usize)g_fb_pitch));
        u32 *p = row + x;
        usize cnt = (usize)w;
        /* REP STOSD is fast even without relying on libc. */
        __asm__ volatile("cld; rep stosl"
                         : "+D"(p), "+c"(cnt)
                         : "a"(color)
                         : "memory");
    }
}

void framebuffer_blit_rect(u32 x, u32 y, u32 width, u32 height, const u32 *src, u32 src_stride) {
    if (!g_fb_ready || !g_fb_draw || !src) {
        return;
    }
    if (width == 0 || height == 0) {
        return;
    }
    if (x >= g_fb_width || y >= g_fb_height) {
        return;
    }

    u32 w = width;
    u32 h = height;
    if (x + w > g_fb_width) w = g_fb_width - x;
    if (y + h > g_fb_height) h = g_fb_height - y;

    for (u32 yy = 0; yy < h; yy++) {
        u32 *dst_row = (u32 *)(void *)(g_fb_draw + ((usize)(y + yy) * (usize)g_fb_pitch));
        const u32 *src_row = src + (usize)yy * (usize)src_stride;
        u32 *dst = dst_row + x;
        const u32 *s = src_row;
        usize cnt = (usize)w;
        __asm__ volatile("cld; rep movsl"
                         : "+D"(dst), "+S"(s), "+c"(cnt)
                         :
                         : "memory");
    }
}

void framebuffer_present(void) {
    if (!g_fb_ready || !g_fb_front || !g_fb_draw) {
        return;
    }
    if (g_fb_front == g_fb_draw) {
        return;
    }
    kmemcpy(g_fb_front, g_fb_draw, (usize)g_fb_pitch * (usize)g_fb_height);
}

void framebuffer_present_rect(u32 x, u32 y, u32 width, u32 height) {
    if (!g_fb_ready || !g_fb_front || !g_fb_draw) {
        return;
    }
    if (g_fb_front == g_fb_draw) {
        return;
    }
    if (width == 0 || height == 0) {
        return;
    }
    if (x >= g_fb_width || y >= g_fb_height) {
        return;
    }

    u32 w = width;
    u32 h = height;
    if (x + w > g_fb_width) {
        w = g_fb_width - x;
    }
    if (y + h > g_fb_height) {
        h = g_fb_height - y;
    }

    const usize bpp = 4;
    const usize row_bytes = (usize)w * bpp;
    for (u32 yy = 0; yy < h; yy++) {
        const usize off = ((usize)(y + yy) * (usize)g_fb_pitch) + ((usize)x * bpp);
        kmemcpy(g_fb_front + off, g_fb_draw + off, row_bytes);
    }
}

void framebuffer_damage_full(void) {
    if (!g_fb_ready) {
        return;
    }
    g_damage_any = true;
    g_damage_full = true;
    g_damage_x0 = 0;
    g_damage_y0 = 0;
    g_damage_x1 = g_fb_width;
    g_damage_y1 = g_fb_height;
}

void framebuffer_damage_rect(i32 x, i32 y, i32 width, i32 height) {
    if (!g_fb_ready) {
        return;
    }
    if (width <= 0 || height <= 0) {
        return;
    }
    i32 x0 = x;
    i32 y0 = y;
    i32 x1 = x + width;
    i32 y1 = y + height;
    if (x1 <= 0 || y1 <= 0) {
        return;
    }
    if (x0 >= (i32)g_fb_width || y0 >= (i32)g_fb_height) {
        return;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (i32)g_fb_width) x1 = (i32)g_fb_width;
    if (y1 > (i32)g_fb_height) y1 = (i32)g_fb_height;
    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    if (!g_damage_any) {
        g_damage_any = true;
        g_damage_full = false;
        g_damage_x0 = (u32)x0;
        g_damage_y0 = (u32)y0;
        g_damage_x1 = (u32)x1;
        g_damage_y1 = (u32)y1;
        return;
    }

    if (g_damage_full) {
        return;
    }

    if ((u32)x0 < g_damage_x0) g_damage_x0 = (u32)x0;
    if ((u32)y0 < g_damage_y0) g_damage_y0 = (u32)y0;
    if ((u32)x1 > g_damage_x1) g_damage_x1 = (u32)x1;
    if ((u32)y1 > g_damage_y1) g_damage_y1 = (u32)y1;
}

bool framebuffer_damage_get(u32 *x, u32 *y, u32 *width, u32 *height, bool *full) {
    if (!g_fb_ready || !g_damage_any) {
        if (full) *full = false;
        return false;
    }
    if (full) *full = g_damage_full;
    if (x) *x = g_damage_x0;
    if (y) *y = g_damage_y0;
    if (width) *width = (g_damage_x1 > g_damage_x0) ? (g_damage_x1 - g_damage_x0) : 0;
    if (height) *height = (g_damage_y1 > g_damage_y0) ? (g_damage_y1 - g_damage_y0) : 0;
    return true;
}

void framebuffer_damage_clear(void) {
    g_damage_any = false;
    g_damage_full = false;
    g_damage_x0 = 0;
    g_damage_y0 = 0;
    g_damage_x1 = 0;
    g_damage_y1 = 0;
}

void framebuffer_init(void) {
    g_fb_front = 0;
    g_fb_draw = 0;
    g_fb_back = 0;
    g_fb_pitch = 0;
    g_fb_width = 0;
    g_fb_height = 0;
    g_fb_ready = false;
    framebuffer_damage_clear();

    const mb2_framebuffer_tag_t *fb = find_fb_tag();
    if (!fb || fb->framebuffer_bpp != 32) {
        log_write("gfx: framebuffer unavailable");
        return;
    }

    /* Boot paging currently identity-maps only the first 1 GiB.
       Reject framebuffers outside that range to avoid page faults/reboots. */
    if ((usize)fb->framebuffer_addr >= g_boot_identity_map_limit) {
        log_write("gfx: framebuffer outside mapped range");
        return;
    }

    g_fb_front = (u8 *)(usize)fb->framebuffer_addr;
    g_fb_pitch = fb->framebuffer_pitch;
    g_fb_width = fb->framebuffer_width;
    g_fb_height = fb->framebuffer_height;
    g_fb_ready = true;

    /* Double-buffer to avoid tearing/black stripes while redrawing. */
    {
        const usize bytes = (usize)g_fb_pitch * (usize)g_fb_height;
        g_fb_back = (u8 *)kmalloc(bytes);
        if (g_fb_back) {
            kmemcpy(g_fb_back, g_fb_front, bytes);
            g_fb_draw = g_fb_back;
            log_write("gfx: double buffer enabled");
        } else {
            g_fb_draw = g_fb_front;
            log_write("gfx: double buffer disabled (oom)");
        }
    }

    log_write("gfx: framebuffer initialized");
}
