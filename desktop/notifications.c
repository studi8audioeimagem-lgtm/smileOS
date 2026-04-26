#include "kernel.h"
#include "../gfx/colors.h"
#include "string.h"

typedef struct {
    char msg[64];
    u64 until_tick;
    bool active;
} toast_t;

static toast_t g_toast;
static char g_hist[16][64];
static u8 g_hist_count;
static bool g_center_open;
static u64 g_center_tick;

void notifications_init(void) {
    kmemset(&g_toast, 0, sizeof(g_toast));
    kmemset(g_hist, 0, sizeof(g_hist));
    g_hist_count = 0;
    g_center_open = false;
    /* Initialize "in the past" so a closed panel doesn't draw during early boot ticks. */
    g_center_tick = ~0ULL;
    log_write("desktop: notifications initialized");
}

void notifications_post(const char *msg) {
    if (!msg) {
        return;
    }
    os_settings_t *s = settings_get();
    if (s->dnd) {
        return;
    }

    /* Small notification sound (PC speaker). */
    sound_beep(880, 30);
    kmemset(g_toast.msg, 0, sizeof(g_toast.msg));
    /* Truncate safely. */
    usize i = 0;
    for (; msg[i] != 0 && i + 1 < sizeof(g_toast.msg); i++) {
        g_toast.msg[i] = msg[i];
    }
    g_toast.msg[i] = 0;
    g_toast.until_tick = scheduler_ticks() + 250ULL; /* ~5s */
    g_toast.active = true;

    /* Store in history (ring). */
    if (g_hist_count < 16) {
        kmemcpy(g_hist[g_hist_count], g_toast.msg, sizeof(g_hist[g_hist_count]));
        g_hist_count++;
    } else {
        for (u8 k = 1; k < 16; k++) {
            kmemcpy(g_hist[k - 1], g_hist[k], sizeof(g_hist[k]));
        }
        kmemcpy(g_hist[15], g_toast.msg, sizeof(g_hist[15]));
    }
}

void notifications_toggle_center(void) {
    g_center_open = !g_center_open;
    g_center_tick = scheduler_ticks();
}

bool notifications_center_is_open(void) {
    return g_center_open;
}

bool notifications_needs_redraw(void) {
    const u64 now = scheduler_ticks();
    if (g_toast.active && now <= g_toast.until_tick) {
        return true;
    }
    const u64 dt = now - g_center_tick;
    if (dt < 22) {
        return true;
    }
    return g_center_open;
}

void notifications_draw(void) {
    if (!framebuffer_ready()) {
        return;
    }

    const u64 now = scheduler_ticks();

    const u32 w = framebuffer_width();
    const u32 h = framebuffer_height();
    /* Toast */
    if (g_toast.active) {
        if (now > g_toast.until_tick) {
            g_toast.active = false;
        } else {
            const u32 box_w = (w > 520) ? 280 : (w - 20);
            const u32 box_h = 44;
            const u32 x = (w > (box_w + 12)) ? (w - box_w - 12) : 0;
            const u32 y0 = (h > (box_h + 58)) ? (h - box_h - 58) : 0;
            /* Pop-in animation */
            const u64 age = (now + 1 > g_toast.until_tick - 250ULL) ? (now - (g_toast.until_tick - 250ULL)) : 0;
            const u32 t = (age > 14) ? 255 : (u32)(age * 255 / 14);
            const u32 y = y0 + (u32)((14 - (i32)age) > 0 ? (14 - (i32)age) : 0);
            const u8 a = (u8)((t * 235U) / 255U);

            draw_blur_rect(x, y, box_w, box_h, 3);
            draw_shadow_round_rect(x, y, box_w, box_h, 12);
            draw_round_rect(x, y, box_w, box_h, 12, COLOR_PANEL_BORDER);
            draw_round_rect_alpha(x + 1, y + 1, box_w - 2, box_h - 2, 11, COLOR_PANEL, a);
            text_draw(x + 12, y + 16, g_toast.msg, COLOR_WHITE);
        }
    }

    /* Notification center (slide-in) */
    {
        const u32 panel_w = (w > 700) ? 320 : (w - 30);
        const u32 panel_h = (h > 420) ? 360 : (h - 60);
        const u32 px0 = w;
        const u32 py = 34;

        const u64 dt = now - g_center_tick;
        u32 prog = (dt > 18) ? 255 : (u32)(dt * 255 / 18);
        if (!g_center_open) {
            prog = 255 - prog;
        }
        if (prog == 0) {
            return;
        }

        const u32 x = px0 - (panel_w * prog) / 255U - 10;
        const u32 y = py;

        /* Cheap blur behind panel */
        draw_blur_rect(x, y, panel_w, panel_h, 3);
        draw_shadow_round_rect(x, y, panel_w, panel_h, 14);
        draw_round_rect_alpha(x, y, panel_w, panel_h, 14, COLOR_PANEL, 190);
        draw_round_rect(x, y, panel_w, panel_h, 14, COLOR_PANEL_BORDER);

        text_draw(x + 14, y + 14, "NOTIFICATIONS", COLOR_WHITE);
        os_settings_t *s = settings_get();
        text_draw(x + panel_w - 90, y + 14, s->dnd ? "DND: ON" : "DND: OFF", s->dnd ? COLOR_WARN : COLOR_SUCCESS);

        u32 yy = y + 40;
        for (u8 i = 0; i < g_hist_count && i < 10; i++) {
            draw_round_rect_alpha(x + 12, yy, panel_w - 24, 26, 10, COLOR_TILE, 150);
            text_draw(x + 18, yy + 9, g_hist[g_hist_count - 1 - i], COLOR_WHITE);
            yy += 30;
            if (yy + 30 >= y + panel_h) break;
        }
    }
}
