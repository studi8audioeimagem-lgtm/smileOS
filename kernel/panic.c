#include "kernel.h"
#include "../gfx/colors.h"

void kernel_panic(const char *reason) {
    log_write("KERNEL PANIC");
    if (reason) {
        log_write(reason);
    }

    if (framebuffer_ready()) {
        framebuffer_fill(0x0B0E14);
        draw_rect(0, 0, framebuffer_width(), 60, COLOR_ERROR);
        text_draw(16, 20, "OOPS! SOMETHING WENT WRONG :(", COLOR_WHITE);
        text_draw(16, 42, "SMILEOS HIT A PROBLEM AND STOPPED.", COLOR_WHITE);
        draw_rect(16, 84, framebuffer_width() - 32, 1, 0x2E3642);
        if (reason) {
            text_draw(16, 104, "DETAILS:", COLOR_WHITE);
            text_draw(16, 120, reason, COLOR_WHITE);
        } else {
            text_draw(16, 104, "DETAILS: (NONE)", COLOR_WHITE);
        }
        text_draw(16, 150, "TIP: CHECK SERIAL LOGS (-serial stdio)", COLOR_SUCCESS);
    }

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
