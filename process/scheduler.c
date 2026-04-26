#include "kernel.h"
#include "types.h"

static u64 g_ticks;

void scheduler_init(void) {
    g_ticks = 0;
    log_write("process: scheduler initialized");
}

void scheduler_tick(void) {
    g_ticks++;
}

u64 scheduler_ticks(void) {
    return g_ticks;
}
