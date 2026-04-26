#include "kernel.h"

typedef void (*init_fn_t)(void);

typedef struct {
    const char *phase_name;
    init_fn_t init_fn;
} init_step_t;

static const init_step_t g_init_steps[] = {
    {"kernel.settings", settings_init},
    {"memory.heap", heap_init},
    {"memory.paging", paging_init},
    {"memory.vmm", vmm_init},
    {"cpu.idt", idt_init},
    {"process.table", process_init},
    {"process.thread", thread_init},
    {"process.scheduler", scheduler_init},
    {"gfx.framebuffer", framebuffer_init},
    {"gfx.draw", draw_init},
    {"gfx.font", font_init},
    {"ui.window_manager", window_manager_init},
    {"ui.window", window_system_init},
    {"ui.render", ui_render_init},
    {"ui.events", ui_events_init},
    {"input.keyboard", keyboard_init},
    {"input.mouse", mouse_init},
    {"input.usb", usb_init},
    {"input.events", input_events_init},
    {"fs.vfs", vfs_init},
    {"fs.files", files_init},
    {"fs.initrd", initrd_init},
    {"services.timer", timer_init},
    {"services.sysinfo", sysinfo_init},
    {"services.sound", sound_init},
    {"services.power", power_init},
    {"services.net", net_init},
    {"apps.loader", apps_loader_init},
    {"apps.api", apps_api_init},
    {"apps.runtime", apps_runtime_init},
    {"theme.theme", theme_init},
    {"theme.icons", icons_init},
    {"theme.fonts", fonts_theme_init},
    {"debug.console", debug_console_init},
    {"debug.memory_view", memory_view_init},
    {"debug.process_view", process_view_init},
    {"desktop.desktop", desktop_init},
    {"desktop.taskbar", taskbar_init},
    {"desktop.startmenu", startmenu_init},
    {"desktop.notifications", notifications_init},
    {"desktop.startscreen", startscreen_init},
    {"ui.cursor", ui_cursor_init},
    {"shell.core", shell_init},
    {"shell.commands", shell_commands_init},
    {"shell.history", shell_history_init},
    {"shell.autocomplete", shell_autocomplete_init},
};

void init_run_all(void) {
    const usize step_count = sizeof(g_init_steps) / sizeof(g_init_steps[0]);

    bootscreen_begin((u32)step_count);
    for (usize i = 0; i < step_count; i++) {
        bootscreen_step(g_init_steps[i].phase_name, (u32)(i + 1));
        log_write(g_init_steps[i].phase_name);
        g_init_steps[i].init_fn();
    }
    bootscreen_end();
}
