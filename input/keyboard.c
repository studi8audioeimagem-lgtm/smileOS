#include "kernel.h"
#include "port.h"
#include "i8042.h"

#define KBD_DATA_PORT 0x60
#define KBD_STATUS_PORT 0x64

typedef enum {
    SCSET_UNKNOWN = 0,
    SCSET_1,
    SCSET_2
} scancode_set_t;

static bool g_extended_code;
static bool g_break_pending;
static bool g_shift;
static bool g_ctrl;
static bool g_alt;
static bool g_altgr;
static bool g_caps;
static scancode_set_t g_scset;
static bool g_ext1;

static bool keyboard_has_data(void) {
    const u8 status = inb(KBD_STATUS_PORT);
    /* Only consume "keyboard" bytes (AUX bit clear). */
    return ((status & 0x01) != 0) && ((status & 0x20) == 0);
}

enum { KEY_NONE = 0 };

static i32 set1_to_keycode(u8 sc, bool shifted, bool caps) {
    /* Common controls. */
    switch (sc) {
        case 0x01: return KEY_ESC;
        case 0x0E: return KEY_BACKSPACE;
        case 0x0F: return KEY_TAB;
        case 0x1C: return '\n';
        case 0x39: return ' ';
        default: break;
    }

    /* Letters (physical layout invariant). */
    switch (sc) {
        case 0x10: return (caps ^ shifted) ? 'Q' : 'q';
        case 0x11: return (caps ^ shifted) ? 'W' : 'w';
        case 0x12: return (caps ^ shifted) ? 'E' : 'e';
        case 0x13: return (caps ^ shifted) ? 'R' : 'r';
        case 0x14: return (caps ^ shifted) ? 'T' : 't';
        case 0x15: return (caps ^ shifted) ? 'Y' : 'y';
        case 0x16: return (caps ^ shifted) ? 'U' : 'u';
        case 0x17: return (caps ^ shifted) ? 'I' : 'i';
        case 0x18: return (caps ^ shifted) ? 'O' : 'o';
        case 0x19: return (caps ^ shifted) ? 'P' : 'p';

        case 0x1E: return (caps ^ shifted) ? 'A' : 'a';
        case 0x1F: return (caps ^ shifted) ? 'S' : 's';
        case 0x20: return (caps ^ shifted) ? 'D' : 'd';
        case 0x21: return (caps ^ shifted) ? 'F' : 'f';
        case 0x22: return (caps ^ shifted) ? 'G' : 'g';
        case 0x23: return (caps ^ shifted) ? 'H' : 'h';
        case 0x24: return (caps ^ shifted) ? 'J' : 'j';
        case 0x25: return (caps ^ shifted) ? 'K' : 'k';
        case 0x26: return (caps ^ shifted) ? 'L' : 'l';

        case 0x2C: return (caps ^ shifted) ? 'Z' : 'z';
        case 0x2D: return (caps ^ shifted) ? 'X' : 'x';
        case 0x2E: return (caps ^ shifted) ? 'C' : 'c';
        case 0x2F: return (caps ^ shifted) ? 'V' : 'v';
        case 0x30: return (caps ^ shifted) ? 'B' : 'b';
        case 0x31: return (caps ^ shifted) ? 'N' : 'n';
        case 0x32: return (caps ^ shifted) ? 'M' : 'm';
        default: break;
    }

    /* PT-BR ABNT2-ish punctuation (best-effort). */
    switch (sc) {
        case 0x02: return shifted ? '!' : '1';
        case 0x03: return shifted ? '@' : '2';
        case 0x04: return shifted ? '#' : '3';
        case 0x05: return shifted ? '$' : '4';
        case 0x06: return shifted ? '%' : '5';
        case 0x07: return shifted ? (i32)0xA8 : '6'; /* ¨ on shift-6 in ABNT2 */
        case 0x08: return shifted ? '&' : '7';
        case 0x09: return shifted ? '*' : '8';
        case 0x0A: return shifted ? '(' : '9';
        case 0x0B: return shifted ? ')' : '0';
        case 0x0C: return shifted ? '_' : '-';
        case 0x0D: return shifted ? '+' : '=';

        case 0x1A: return shifted ? '`' : (i32)0xB4; /* key near P: ´ / ` */
        case 0x1B: return shifted ? '{' : '[';

        case 0x27: return shifted ? (i32)0xC7 : (i32)0xE7; /* Ç/ç at ; position */
        case 0x28: return shifted ? '^' : '~';
        case 0x2B: return shifted ? '}' : ']';

        case 0x33: return shifted ? '<' : ',';
        case 0x34: return shifted ? '>' : '.';
        case 0x35: return shifted ? ':' : ';';
        case 0x29: return shifted ? '"' : '\'';
        default: break;
    }

    return KEY_NONE;
}

static i32 set1_ext_to_keycode(u8 sc) {
    /* Set 1 extended codes commonly used by arrows/home/end/etc. */
    switch (sc) {
        case 0x48: return KEY_UP;
        case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x53: return KEY_DELETE;
        case 0x47: return KEY_HOME;
        case 0x4F: return KEY_END;
        case 0x49: return KEY_PAGEUP;
        case 0x51: return KEY_PAGEDOWN;
        default: return KEY_NONE;
    }
}

static i32 set2_to_keycode(u8 sc, bool shifted, bool caps) {
    switch (sc) {
        case 0x76: return KEY_ESC;
        case 0x66: return KEY_BACKSPACE;
        case 0x0D: return KEY_TAB;
        case 0x5A: return '\n';
        case 0x29: return ' ';
        default: break;
    }

    /* Letters (physical layout is invariant across locales). */
    switch (sc) {
        case 0x15: return (caps ^ shifted) ? 'Q' : 'q';
        case 0x1D: return (caps ^ shifted) ? 'W' : 'w';
        case 0x24: return (caps ^ shifted) ? 'E' : 'e';
        case 0x2D: return (caps ^ shifted) ? 'R' : 'r';
        case 0x2C: return (caps ^ shifted) ? 'T' : 't';
        case 0x35: return (caps ^ shifted) ? 'Y' : 'y';
        case 0x3C: return (caps ^ shifted) ? 'U' : 'u';
        case 0x43: return (caps ^ shifted) ? 'I' : 'i';
        case 0x44: return (caps ^ shifted) ? 'O' : 'o';
        case 0x4D: return (caps ^ shifted) ? 'P' : 'p';

        case 0x1C: return (caps ^ shifted) ? 'A' : 'a';
        case 0x1B: return (caps ^ shifted) ? 'S' : 's';
        case 0x23: return (caps ^ shifted) ? 'D' : 'd';
        case 0x2B: return (caps ^ shifted) ? 'F' : 'f';
        case 0x34: return (caps ^ shifted) ? 'G' : 'g';
        case 0x33: return (caps ^ shifted) ? 'H' : 'h';
        case 0x3B: return (caps ^ shifted) ? 'J' : 'j';
        case 0x42: return (caps ^ shifted) ? 'K' : 'k';
        case 0x4B: return (caps ^ shifted) ? 'L' : 'l';

        case 0x1A: return (caps ^ shifted) ? 'Z' : 'z';
        case 0x22: return (caps ^ shifted) ? 'X' : 'x';
        case 0x21: return (caps ^ shifted) ? 'C' : 'c';
        case 0x2A: return (caps ^ shifted) ? 'V' : 'v';
        case 0x32: return (caps ^ shifted) ? 'B' : 'b';
        case 0x31: return (caps ^ shifted) ? 'N' : 'n';
        case 0x3A: return (caps ^ shifted) ? 'M' : 'm';
        default: break;
    }

    /* PT-BR ABNT2-ish punctuation (best-effort; dead key composition not implemented). */
    switch (sc) {
        case 0x0E: return shifted ? '"' : '\'';

        case 0x16: return shifted ? '!' : '1';
        case 0x1E: return shifted ? '@' : '2';
        case 0x26: return shifted ? '#' : '3';
        case 0x25: return shifted ? '$' : '4';
        case 0x2E: return shifted ? '%' : '5';
        case 0x36: return shifted ? (i32)0xA8 : '6';
        case 0x3D: return shifted ? '&' : '7';
        case 0x3E: return shifted ? '*' : '8';
        case 0x46: return shifted ? '(' : '9';
        case 0x45: return shifted ? ')' : '0';
        case 0x4E: return shifted ? '_' : '-';
        case 0x55: return shifted ? '+' : '=';

        case 0x54: return shifted ? '`' : (i32)0xB4; /* key near P */
        case 0x5B: return shifted ? '{' : '[';
        case 0x5D: return shifted ? '}' : ']';

        case 0x4C: return shifted ? (i32)0xC7 : (i32)0xE7; /* Ç/ç */
        case 0x52: return shifted ? '^' : '~';
        case 0x41: return shifted ? '<' : ',';
        case 0x49: return shifted ? '>' : '.';
        case 0x4A: return shifted ? ':' : ';';
        default: break;
    }

    return KEY_NONE;
}

static i32 set2_ext_to_keycode(u8 sc) {
    switch (sc) {
        case 0x75: return KEY_UP;
        case 0x72: return KEY_DOWN;
        case 0x6B: return KEY_LEFT;
        case 0x74: return KEY_RIGHT;
        case 0x71: return KEY_DELETE;
        case 0x6C: return KEY_HOME;
        case 0x69: return KEY_END;
        case 0x7D: return KEY_PAGEUP;
        case 0x7A: return KEY_PAGEDOWN;
        default: return KEY_NONE;
    }
}

void keyboard_init(void) {
    g_extended_code = false;
    g_break_pending = false;
    g_shift = false;
    g_ctrl = false;
    g_alt = false;
    g_altgr = false;
    g_caps = false;
    g_scset = SCSET_UNKNOWN;
    g_ext1 = false;

    /* Force i8042 translation OFF; we decode set-2 scancodes directly. */
    u8 cmd = i8042_read_command_byte();
    cmd = (u8)(cmd & (u8)~0x40);
    i8042_write_command_byte(cmd);

    /* If translation couldn't be disabled, fall back to set-1 decoding. */
    u8 cmd2 = i8042_read_command_byte();
    g_scset = (cmd2 & 0x40) ? SCSET_1 : SCSET_2;

    log_write("input: keyboard initialized");
}

void keyboard_poll(void) {
    while (keyboard_has_data()) {
        extern void keyboard_handle_byte(u8 sc);
        keyboard_handle_byte(inb(KBD_DATA_PORT));
    }
}

void keyboard_handle_byte(u8 sc) {
    if (g_scset == SCSET_1) {
        if (sc == 0xE0) {
            g_ext1 = true;
            return;
        }

        const bool released = (sc & 0x80) != 0;
        const u8 key = (u8)(sc & 0x7F);

        if (g_ext1) {
            const i32 kc = set1_ext_to_keycode(key);
            if (kc != KEY_NONE) {
                input_event_t evt;
                evt.type = INPUT_EVENT_KEY;
                evt.a = kc;
                evt.b = released ? 0 : 1;
                evt.c = 1;
                input_event_push(evt);
            }
            g_ext1 = false;
            return;
        }

        /* Modifier tracking (set 1). */
        if (key == 0x2A || key == 0x36) { /* LShift/RShift */
            g_shift = released ? false : true;
            return;
        }
        if (key == 0x1D) { /* Ctrl */
            g_ctrl = released ? false : true;
            return;
        }
        if (key == 0x38) { /* Alt */
            g_alt = released ? false : true;
            return;
        }
        if (key == 0x3A && !released) { /* CapsLock */
            g_caps = !g_caps;
            return;
        }

        const i32 kc = set1_to_keycode(key, g_shift, g_caps);
        if (kc == KEY_NONE) {
            return;
        }

        input_event_t evt;
        evt.type = INPUT_EVENT_KEY;
        evt.a = kc;
        evt.b = released ? 0 : 1;
        evt.c = 0;
        input_event_push(evt);
        return;
    }

    /* Default: set 2. */
    if (sc == 0xF0) {
        g_break_pending = true;
        return;
    }
    if (sc == 0xE0) {
        g_extended_code = true;
        return;
    }

    const bool released = g_break_pending;
    g_break_pending = false;
    const u8 key = sc;

    if (g_extended_code) {
        i32 keycode = set2_ext_to_keycode(key);
        if (keycode == KEY_NONE) {
            g_extended_code = false;
            return;
        }
        input_event_t evt;
        evt.type = INPUT_EVENT_KEY;
        evt.a = keycode;
        evt.b = released ? 0 : 1;
        evt.c = 1;
        input_event_push(evt);
        g_extended_code = false;
        return;
    }

    /* Modifier tracking (set 2). */
    if (key == 0x12 || key == 0x59) { /* LShift/RShift */
        g_shift = released ? false : true;
        return;
    }
    if (key == 0x14) { /* Ctrl */
        g_ctrl = released ? false : true;
        return;
    }
    if (key == 0x11) { /* Alt */
        g_alt = released ? false : true;
        return;
    }
    if (key == 0x58 && !released) { /* CapsLock */
        g_caps = !g_caps;
        return;
    }

    i32 keycode = set2_to_keycode(key, g_shift, g_caps);
    if (keycode == KEY_NONE) {
        g_extended_code = false;
        return;
    }

    input_event_t evt;
    evt.type = INPUT_EVENT_KEY;
    evt.a = keycode;            /* key */
    evt.b = released ? 0 : 1;   /* pressed */
    evt.c = g_extended_code ? 1 : 0;
    input_event_push(evt);

    g_extended_code = false;
}
