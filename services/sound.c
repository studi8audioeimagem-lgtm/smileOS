#include "kernel.h"
#include "port.h"

void sound_init(void) {
    log_write("services: sound abstraction initialized");
}

/* PC speaker (works in QEMU + real hardware). */

#define PIT_CH2  0x42
#define PIT_CMD  0x43
#define SPK_PORT 0x61

static bool g_beeping;
static u64 g_beep_until;

static void speaker_on(u32 freq_hz) {
    if (freq_hz < 20) freq_hz = 20;
    if (freq_hz > 20000) freq_hz = 20000;

    const u32 div = 1193182U / freq_hz;

    outb(PIT_CMD, 0xB6); /* ch2, lobyte/hibyte, square wave */
    outb(PIT_CH2, (u8)(div & 0xFF));
    outb(PIT_CH2, (u8)((div >> 8) & 0xFF));

    u8 v = inb(SPK_PORT);
    v |= 0x03; /* enable speaker + gate */
    outb(SPK_PORT, v);
    g_beeping = true;
}

static void speaker_off(void) {
    u8 v = inb(SPK_PORT);
    v &= (u8)~0x03;
    outb(SPK_PORT, v);
    g_beeping = false;
}

void sound_beep(u32 freq_hz, u32 ms) {
    speaker_on(freq_hz);
    const u64 ticks = scheduler_ticks();
    const u64 dt = (ms * 50ULL) / 1000ULL;
    g_beep_until = ticks + (dt ? dt : 1ULL);
}

void sound_poll(void) {
    if (g_beeping) {
        if (scheduler_ticks() >= g_beep_until) {
            speaker_off();
        }
    }
}
