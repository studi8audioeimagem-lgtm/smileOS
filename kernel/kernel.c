#include "kernel.h"

u64 g_multiboot2_info_ptr;

static void kernel_init_pipeline(void) {
    log_init();
    log_write("smileOS: kernel init start");
    init_run_all();

    log_write("smileOS: kernel init complete");
}

void kernel_main(u64 multiboot2_info_ptr) {
    g_multiboot2_info_ptr = multiboot2_info_ptr;
    kernel_init_pipeline();

    for (;;) {
        input_poll();
        net_poll();
        sound_poll();
        ui_events_pump();
        scheduler_tick();
        ui_animate_tick();
        /* Don't use HLT yet: interrupts are still disabled and no IDT is set up,
           so HLT would stop the polling loop and make input appear "frozen". */
        for (volatile usize i = 0; i < 50000; i++) {
            __asm__ volatile ("pause");
        }
    }
}
