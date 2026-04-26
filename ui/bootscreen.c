#include "kernel.h"
#include "types.h"
#include "string.h"
#include "../gfx/font_atlas.h"

enum { BOOTSCREEN_MAX_STEPS = 96 };
enum {
    /* Tuning knobs for "realistic" boot feel. These intentionally burn time. */
    BOOTSCREEN_DELAY_ITERS = 520000,
    BOOTSCREEN_FRAMES_REPLAY_STEP = 10,
    BOOTSCREEN_FRAMES_PER_STEP = 16
};

static bool g_enabled;
static u32 g_total;
static u32 g_cur; /* 1-based */
static u32 g_count;
static char g_steps[BOOTSCREEN_MAX_STEPS][48];

static bool g_started_draw;
static u32 g_spinner;
static u32 g_pending_count;
static char g_pending[BOOTSCREEN_MAX_STEPS][48];

static void bootscreen_draw_background(void) {
    if (!framebuffer_ready()) return;
    const u32 w = framebuffer_width();
    const u32 h = framebuffer_height();
    if (w == 0 || h == 0) return;

    /* Dark, modern-ish vertical gradient. */
    const u32 top = 0x060B14;
    const u32 mid = 0x0C1630;
    const u32 bot = 0x03060C;

    for (u32 y = 0; y < h; y++) {
        const u32 t = (y * 255U) / (h ? h : 1);
        u32 c;
        if (t < 160) {
            const u32 tt = (t * 255U) / 160U;
            const u32 r = (((top >> 16) & 0xFFU) * (255U - tt) + ((mid >> 16) & 0xFFU) * tt) / 255U;
            const u32 g = (((top >> 8) & 0xFFU) * (255U - tt) + ((mid >> 8) & 0xFFU) * tt) / 255U;
            const u32 b = ((top & 0xFFU) * (255U - tt) + (mid & 0xFFU) * tt) / 255U;
            c = (r << 16) | (g << 8) | b;
        } else {
            const u32 tt = ((t - 160U) * 255U) / 95U;
            const u32 r = (((mid >> 16) & 0xFFU) * (255U - tt) + ((bot >> 16) & 0xFFU) * tt) / 255U;
            const u32 g = (((mid >> 8) & 0xFFU) * (255U - tt) + ((bot >> 8) & 0xFFU) * tt) / 255U;
            const u32 b = ((mid & 0xFFU) * (255U - tt) + (bot & 0xFFU) * tt) / 255U;
            c = (r << 16) | (g << 8) | b;
        }
        framebuffer_fill_rect_fast(0, y, w, 1, c);
    }
}

static void fake_delay(void) {
    /* Tiny busy wait; keeps boot feeling "alive" without making it painfully slow on TCG. */
    for (volatile u32 i = 0; i < (u32)BOOTSCREEN_DELAY_ITERS; i++) {
        __asm__ volatile ("pause");
    }
}

static void fake_delay_extra(const char *phase) {
    /* Add a little deterministic jitter so it feels less "same duration every step". */
    if (!phase) return;
    u32 acc = 0;
    for (usize i = 0; phase[i] != 0; i++) {
        acc = (acc * 33U) ^ (u32)(unsigned char)phase[i];
    }
    /* 0..2 extra delays */
    const u32 extra = acc % 3U;
    for (u32 i = 0; i < extra; i++) fake_delay();
}

static void phase_to_path(const char *phase, char *out, usize cap) {
    if (!out || cap == 0) return;
    kmemset(out, 0, cap);
    if (!phase) return;

    usize o = 0;
    for (usize i = 0; phase[i] != 0 && o + 1 < cap; i++) {
        char c = phase[i];
        if (c == '.') c = '/';
        out[o++] = c;
    }
    if (o + 2 < cap) {
        out[o++] = '.';
        out[o++] = 'c';
    }
    out[o] = 0;
}

static void bootscreen_draw(void) {
    if (!g_enabled || !framebuffer_ready()) return;

    bootscreen_draw_background();

    const u32 w = framebuffer_width();
    const u32 h = framebuffer_height();

    /* Header */
    text_draw(18, 16, "SMILEOS BOOT", 0xFFFFFF);

    /* Progress (simple bar) */
    u32 bar_w = (w > 80) ? (w - 36) : w;
    if (bar_w > 520) bar_w = 520;
    const u32 bar_x = 18;
    const u32 bar_y = 34;
    const u32 bar_h = 6;
    framebuffer_fill_rect_fast(bar_x, bar_y, bar_w, bar_h, 0x1B253D);
    u32 fill = 0;
    if (g_total) {
        fill = (u32)(((u64)bar_w * (u64)g_cur) / (u64)g_total);
    }
    if (fill > bar_w) fill = bar_w;
    framebuffer_fill_rect_fast(bar_x, bar_y, fill, bar_h, settings_accent_color());

    /* "executing ..." line */
    {
        char curline[96];
        kmemset(curline, 0, sizeof(curline));
        const char *pfx = "compiling ";
        usize o = 0;
        for (usize i = 0; pfx[i] != 0 && o + 1 < sizeof(curline); i++) curline[o++] = pfx[i];

        char path[56];
        const char *name = (g_count && g_cur && g_cur <= g_count) ? g_steps[g_cur - 1] : 0;
        if (!name) name = "kernel";
        phase_to_path(name, path, sizeof(path));
        for (usize i = 0; path[i] != 0 && o + 1 < sizeof(curline); i++) curline[o++] = path[i];
        curline[o] = 0;
        text_draw(18, 48, curline, 0xFFFFFF);
    }

    /* Simple spinner near the header (gives a "working" vibe). */
    {
        const char *spin = "|/-\\";
        char s[2];
        s[0] = spin[g_spinner & 3U];
        s[1] = 0;
        text_draw(140, 16, s, 0xB7C3D9);
    }

    /* Step list */
    {
        const u32 list_x = 18;
        const u32 list_y = 68;
        const u32 line_h = 11;
        const u32 max_lines = (h > list_y + 24) ? ((h - list_y - 24) / line_h) : 0;
        if (max_lines) {
            const u32 show = (g_count < max_lines) ? g_count : max_lines;
            const u32 start = (g_count > show) ? (g_count - show) : 0;
            for (u32 i = 0; i < show; i++) {
                const u32 idx = start + i;
                char line[64];
                kmemset(line, 0, sizeof(line));
                const char *pfx = (idx + 1 == g_cur) ? "> " : "  ";
                usize o = 0;
                for (usize j = 0; pfx[j] && o + 1 < sizeof(line); j++) line[o++] = pfx[j];
                char path[56];
                phase_to_path(g_steps[idx], path, sizeof(path));
                for (usize j = 0; path[j] && o + 1 < sizeof(line); j++) line[o++] = path[j];
                line[o] = 0;
                const u32 col = (idx + 1 == g_cur) ? 0xEAF2FF : 0xB7C3D9;
                text_draw(list_x, list_y + i * line_h, line, col);
            }
        }
    }

    /* Footer: step counter */
    {
        char buf[32];
        kmemset(buf, 0, sizeof(buf));
        buf[0] = 's'; buf[1] = 't'; buf[2] = 'e'; buf[3] = 'p'; buf[4] = ' ';
        /* g_cur and g_total are small; do simple decimal. */
        u32 a = g_cur;
        u32 b = g_total;
        char tmp[12];
        usize ti = 0;
        do { tmp[ti++] = (char)('0' + (a % 10U)); a /= 10U; } while (a && ti < sizeof(tmp));
        usize o = 5;
        while (ti && o + 1 < sizeof(buf)) buf[o++] = tmp[--ti];
        if (o + 1 < sizeof(buf)) buf[o++] = '/';
        ti = 0;
        do { tmp[ti++] = (char)('0' + (b % 10U)); b /= 10U; } while (b && ti < sizeof(tmp));
        while (ti && o + 1 < sizeof(buf)) buf[o++] = tmp[--ti];
        buf[o] = 0;
        text_draw(18, (h > 14) ? (h - 14) : 0, buf, 0xB7C3D9);
    }

    framebuffer_present();
}

void bootscreen_begin(u32 total_steps) {
    g_enabled = true;
    g_total = total_steps;
    g_cur = 0;
    g_count = 0;
    g_started_draw = false;
    g_spinner = 0;
    g_pending_count = 0;
    kmemset(g_steps, 0, sizeof(g_steps));
    kmemset(g_pending, 0, sizeof(g_pending));

    /* Use a smaller font for the fake "compile" boot screen. */
    text_set_face(&g_font_face_boot);
}

void bootscreen_step(const char *phase_name, u32 step_index_1based) {
    if (!g_enabled || !phase_name) {
        return;
    }

    /* If there's no framebuffer yet, queue the early steps so we can "compile" them later
       instead of dumping a huge list instantly when the UI comes up. */
    if (!framebuffer_ready()) {
        if (g_pending_count < BOOTSCREEN_MAX_STEPS) {
            kmemset(g_pending[g_pending_count], 0, sizeof(g_pending[g_pending_count]));
            for (usize i = 0; phase_name[i] != 0 && i + 1 < sizeof(g_pending[g_pending_count]); i++) {
                g_pending[g_pending_count][i] = phase_name[i];
            }
            g_pending_count++;
        }
        g_cur = step_index_1based;
        return;
    }

    /* First time the framebuffer is ready: replay queued steps with a small delay
       so it feels like a compile phase rather than an instant spam dump. */
    if (!g_started_draw) {
        g_started_draw = true;
        for (u32 i = 0; i < g_pending_count; i++) {
            if (g_count >= BOOTSCREEN_MAX_STEPS) break;
            kmemcpy(g_steps[g_count], g_pending[i], sizeof(g_steps[g_count]));
            g_count++;
            g_cur = g_count;
            for (u32 f = 0; f < (u32)BOOTSCREEN_FRAMES_REPLAY_STEP; f++) {
                g_spinner++;
                bootscreen_draw();
                fake_delay();
            }
            fake_delay_extra(g_pending[i]);
        }
        g_pending_count = 0;
    }

    if (g_count < BOOTSCREEN_MAX_STEPS) {
        kmemset(g_steps[g_count], 0, sizeof(g_steps[g_count]));
        for (usize i = 0; phase_name[i] != 0 && i + 1 < sizeof(g_steps[g_count]); i++) {
            g_steps[g_count][i] = phase_name[i];
        }
        g_count++;
    }

    g_cur = step_index_1based;
    if (g_cur > g_count) g_cur = g_count;

    /* Animate this step a little. */
    for (u32 f = 0; f < (u32)BOOTSCREEN_FRAMES_PER_STEP; f++) {
        g_spinner++;
        bootscreen_draw();
        fake_delay();
    }
    fake_delay_extra(phase_name);
}

void bootscreen_end(void) {
    g_enabled = false;
    text_set_face(&g_font_face_ui);
}
