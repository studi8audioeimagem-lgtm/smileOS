#ifndef KERNEL_H
#define KERNEL_H

#include "types.h"

/* Keycodes for INPUT_EVENT_KEY (evt.a). Printable ASCII uses its character code. */
typedef enum {
    KEY_BACKSPACE = 0x100,
    KEY_TAB = 0x101,
    KEY_ESC = 0x102,
    KEY_UP = 0x110,
    KEY_DOWN = 0x111,
    KEY_LEFT = 0x112,
    KEY_RIGHT = 0x113,
    KEY_DELETE = 0x114,
    KEY_HOME = 0x115,
    KEY_END = 0x116,
    KEY_PAGEUP = 0x117,
    KEY_PAGEDOWN = 0x118
} keycode_t;

void kernel_main(u64 multiboot2_info_ptr);
void kernel_panic(const char *reason);
void init_run_all(void);

void log_init(void);
void log_write(const char *message);

/* Boot screen (updated during init_run_all). */
void bootscreen_begin(u32 total_steps);
void bootscreen_step(const char *phase_name, u32 step_index_1based);
void bootscreen_end(void);

typedef struct {
    bool tutorial_enabled;
    bool tutorial_completed;
    u8 tutorial_step;
    bool large_icons;
    bool dark_theme;
    bool large_text;
    u8 theme_mode;          /* 0=Light, 1=Dark, 2=Aqua, 3=Graphite */
    u32 accent_color;       /* RGB */
    bool dock_autohide;
    u8 wallpaper_mode;      /* 0=Soft, 1=Scenic (future) */
    bool dnd;
} os_settings_t;

void settings_init(void);
os_settings_t *settings_get(void);
u32 settings_accent_color(void);

void heap_init(void);
void *kmalloc(usize size);
void kfree(void *ptr);

void paging_init(void);
void vmm_init(void);

void scheduler_init(void);
void scheduler_tick(void);
u64 scheduler_ticks(void);
void process_init(void);
void thread_init(void);

void idt_init(void);

void framebuffer_init(void);
bool framebuffer_ready(void);
u32 framebuffer_width(void);
u32 framebuffer_height(void);
u32 framebuffer_pitch(void);
void framebuffer_put_pixel(u32 x, u32 y, u32 color);
u32 framebuffer_get_pixel(u32 x, u32 y);
void framebuffer_fill(u32 color);
void framebuffer_present(void);
void framebuffer_present_rect(u32 x, u32 y, u32 width, u32 height);
/* Fast fills used by wallpaper and large solid panels. */
void framebuffer_fill_rect_fast(u32 x, u32 y, u32 width, u32 height, u32 color);
/* Fast blit into the draw buffer (src_stride is in pixels). */
void framebuffer_blit_rect(u32 x, u32 y, u32 width, u32 height, const u32 *src, u32 src_stride);
/* Damage tracking: modules mark what they redrew; cursor overlay presents efficiently. */
void framebuffer_damage_full(void);
void framebuffer_damage_rect(i32 x, i32 y, i32 width, i32 height);
bool framebuffer_damage_get(u32 *x, u32 *y, u32 *width, u32 *height, bool *full);
void framebuffer_damage_clear(void);
void draw_init(void);
void draw_rect(u32 x, u32 y, u32 width, u32 height, u32 color);
void draw_rect_alpha(u32 x, u32 y, u32 width, u32 height, u32 color, u8 alpha);
void draw_shadow_rect(u32 x, u32 y, u32 width, u32 height);
void draw_round_rect(u32 x, u32 y, u32 width, u32 height, u32 radius, u32 color);
void draw_round_rect_alpha(u32 x, u32 y, u32 width, u32 height, u32 radius, u32 color, u8 alpha);
void draw_shadow_round_rect(u32 x, u32 y, u32 width, u32 height, u32 radius);
void draw_blur_rect(u32 x, u32 y, u32 width, u32 height, u32 step);
void font_init(void);
void text_draw(u32 x, u32 y, const char *s, u32 color);
void text_draw_scaled(u32 x, u32 y, const char *s, u32 color, u32 scale);
u32 text_line_height(void);
u32 text_cell_advance(void);
u32 text_ascent(void);
void text_set_face(const void *face);
const void *text_get_face(void);
u16 palette_size(void);
u32 palette_get(u16 idx);

void window_manager_init(void);
void window_system_init(void);
void ui_render_init(void);
void ui_events_init(void);
void ui_events_pump(void);
void ui_cursor_init(void);
void ui_cursor_begin_overlay(void);
void ui_cursor_end_overlay(void);

void desktop_init(void);
void taskbar_init(void);
void startmenu_init(void);
void notifications_init(void);
void startscreen_init(void);
bool startscreen_is_open(void);
bool startscreen_needs_redraw(void);
void startscreen_draw(void);
/* Partial desktop redraw for low-tear window dragging and overlays. */
void desktop_draw_wallpaper_rect(u32 x, u32 y, u32 width, u32 height);
void desktop_redraw_rect(i32 x, i32 y, i32 width, i32 height);

void desktop_handle_key(i32 keycode, bool pressed);
void ui_animate_tick(void);

void keyboard_init(void);
void keyboard_poll(void);
void mouse_init(void);
void mouse_poll(void);
void usb_init(void);
void usb_poll(void);
void input_events_init(void);
void input_poll(void);

typedef enum {
    INPUT_EVENT_NONE = 0,
    INPUT_EVENT_KEY,
    INPUT_EVENT_MOUSE_MOVE,
    INPUT_EVENT_MOUSE_BUTTON
} input_event_type_t;

typedef struct {
    input_event_type_t type;
    i32 a;
    i32 b;
    i32 c;
} input_event_t;

bool input_event_push(input_event_t event);
bool input_event_pop(input_event_t *event);

typedef enum {
    CURSOR_DEFAULT = 0,
    CURSOR_TEXT,
    CURSOR_DRAG
} cursor_state_t;

void ui_cursor_set_state(cursor_state_t state);
cursor_state_t ui_cursor_get_state(void);
void ui_cursor_get_pos(i32 *x, i32 *y);
bool startscreen_handle_input(const input_event_t *evt, i32 cursor_x, i32 cursor_y);

void desktop_handle_input(const input_event_t *evt, i32 cursor_x, i32 cursor_y);
void desktop_redraw(void);

bool startmenu_is_open(void);
void startmenu_toggle(void);
void startmenu_draw(void);
bool startmenu_handle_input(const input_event_t *evt, i32 cursor_x, i32 cursor_y);

void notifications_post(const char *msg);
void notifications_draw(void);
void notifications_toggle_center(void);
bool notifications_center_is_open(void);
bool notifications_needs_redraw(void);

bool startmenu_needs_redraw(void);
bool dock_needs_redraw(void);

bool dock_handle_input(const input_event_t *evt, i32 cursor_x, i32 cursor_y);

void shell_init(void);
void shell_commands_init(void);
void shell_history_init(void);
void shell_autocomplete_init(void);

void vfs_init(void);
void files_init(void);
void initrd_init(void);

void timer_init(void);
void sysinfo_init(void);
void sound_init(void);
void sound_poll(void);
void sound_beep(u32 freq_hz, u32 ms);
void power_init(void);
void net_init(void);
void net_poll(void);

void apps_loader_init(void);
void apps_api_init(void);
void apps_runtime_init(void);
void apps_open_notepad(void);
void apps_open_bash_terminal(void);
void apps_open_sysconfig(void);
void apps_open_calculator(void);
void apps_open_clock(void);
void apps_open_help(void);
void apps_open_about(void);
void apps_open_palette(void);
void apps_open_typing(void);
void apps_open_files(void);
void apps_open_trash(void);
void apps_open_browser(void);
void apps_handle_key(i32 keycode, bool pressed);
bool apps_has_active_window(void);
bool apps_handle_mouse(const input_event_t *evt, i32 cursor_x, i32 cursor_y);
cursor_state_t apps_cursor_state(i32 cursor_x, i32 cursor_y, bool left_down);
bool apps_has_running_app(void);
const char *apps_running_title(void);
void apps_restore_minimized(void);
void apps_minimize_active(void);
void apps_redraw(void);
bool apps_needs_redraw(void);

void theme_init(void);
void icons_init(void);
void fonts_theme_init(void);

/* UI icons */
/* (Keep the draw API in icons.h; this stays here only for compilation unit reachability.) */

void debug_console_init(void);
void memory_view_init(void);
void process_view_init(void);

#endif
