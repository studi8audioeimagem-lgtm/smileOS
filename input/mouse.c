#include "kernel.h"
#include "port.h"
#include "i8042.h"

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64
#define PS2_CMD_PORT 0x64

static u8 g_packet[4];
static u8 g_packet_index;
static bool g_streaming;
static u8 g_packet_size;
static bool g_has_wheel;
static u8 g_last_buttons;

static void ps2_mouse_write(u8 value) {
    if (!i8042_wait_input_clear()) {
        return;
    }
    outb(PS2_CMD_PORT, 0xD4);
    if (!i8042_wait_input_clear()) {
        return;
    }
    outb(PS2_DATA_PORT, value);
}

static bool ps2_aux_has_data(void) {
    const u8 status = inb(PS2_STATUS_PORT);
    /* Output buffer full + AUX bit set => mouse byte. */
    return ((status & 0x01) != 0) && ((status & 0x20) != 0);
}

static bool ps2_read_aux_byte(u8 *out) {
    if (!out) {
        return false;
    }
    if (!ps2_aux_has_data()) {
        return false;
    }
    *out = inb(PS2_DATA_PORT);
    return true;
}

static bool ps2_expect_aux(u8 expected) {
    u8 value = 0;
    for (usize i = 0; i < 200000; i++) {
        if (ps2_read_aux_byte(&value)) {
            return value == expected;
        }
    }
    return false;
}

static bool ps2_mouse_cmd(u8 cmd) {
    ps2_mouse_write(cmd);
    /* Mouse should ACK with 0xFA. */
    return ps2_expect_aux(0xFA);
}

bool mouse_mid_packet(void) {
    return g_packet_index != 0;
}

static void mouse_process_byte(u8 value) {
    /* Robust resync: first byte must have bit3 set, and overflow bits clear. */
    if (g_packet_index == 0) {
        if ((value & 0x08) == 0) {
            return;
        }
        if ((value & 0xC0) != 0) {
            return;
        }
        g_packet[0] = value;
        g_packet_index = 1;
        return;
    }

    g_packet[g_packet_index++] = value;
    if (g_packet_index < g_packet_size) {
        return;
    }
    g_packet_index = 0;

    i32 dx = (g_packet[0] & 0x10) ? (i32)g_packet[1] - 256 : (i32)g_packet[1];
    i32 dy = (g_packet[0] & 0x20) ? (i32)g_packet[2] - 256 : (i32)g_packet[2];

    /* Occasional electrical/virtual glitches can still slip through; cap per-packet deltas. */
    if (dx > 50) dx = 50;
    if (dx < -50) dx = -50;
    if (dy > 50) dy = 50;
    if (dy < -50) dy = -50;

    if (dx != 0 || dy != 0) {
        input_event_t move_evt;
        move_evt.type = INPUT_EVENT_MOUSE_MOVE;
        move_evt.a = dx;
        move_evt.b = -dy;
        move_evt.c = 0;
        input_event_push(move_evt);
    }

    /* Push button state only when it changes (avoids event-queue overflow/ghost dragging). */
    const u8 buttons = (u8)(g_packet[0] & 0x07);
    if (buttons != g_last_buttons) {
        g_last_buttons = buttons;
        input_event_t btn_evt;
        btn_evt.type = INPUT_EVENT_MOUSE_BUTTON;
        btn_evt.a = (buttons & 0x01) ? 1 : 0; /* left */
        btn_evt.b = (buttons & 0x02) ? 1 : 0; /* right */
        btn_evt.c = (buttons & 0x04) ? 1 : 0; /* middle */
        input_event_push(btn_evt);
    }

    /* Optional wheel byte (IntelliMouse). Currently ignored. */
    if (g_has_wheel) {
        (void)g_packet[3];
    }
}

void mouse_handle_byte(u8 value) {
    if (!g_streaming) {
        return;
    }
    mouse_process_byte(value);
}

void mouse_init(void) {
    g_packet_index = 0;
    g_streaming = false;
    g_packet_size = 3;
    g_has_wheel = false;
    g_last_buttons = 0;

    if (!i8042_wait_input_clear()) {
        log_write("input: mouse init failed (controller busy)");
        return;
    }
    outb(PS2_CMD_PORT, 0xA8); /* enable auxiliary device */
    i8042_drain_output();

    /* Keep i8042 translation OFF so the keyboard driver can consume set-2 scancodes directly. */
    u8 cmd = i8042_read_command_byte();
    cmd = (u8)(cmd & (u8)~0x40);
    i8042_write_command_byte(cmd);

    /* Set defaults, then try to enable IntelliMouse wheel mode (best-effort). */
    (void)ps2_mouse_cmd(0xF6); /* set defaults */

    /* IntelliMouse sequence: Set sample rate 200, 100, 80; then read device ID. */
    (void)ps2_mouse_cmd(0xF3);
    ps2_mouse_write(200);
    (void)ps2_expect_aux(0xFA);
    (void)ps2_mouse_cmd(0xF3);
    ps2_mouse_write(100);
    (void)ps2_expect_aux(0xFA);
    (void)ps2_mouse_cmd(0xF3);
    ps2_mouse_write(80);
    (void)ps2_expect_aux(0xFA);

    /* Read ID (0x00 standard, 0x03 wheel). */
    u8 id = 0;
    if (ps2_mouse_cmd(0xF2)) {
        for (usize i = 0; i < 200000; i++) {
            if (ps2_read_aux_byte(&id)) {
                break;
            }
        }
    }
    if (id == 0x03) {
        g_packet_size = 4;
        g_has_wheel = true;
        log_write("input: ps/2 mouse wheel mode enabled");
    }

    if (!ps2_mouse_cmd(0xF4)) { /* enable streaming */
        log_write("input: mouse init failed (no ACK)");
        return;
    }
    g_streaming = true;

    log_write("input: mouse initialized");
}

void mouse_poll(void) {
    if (!g_streaming) {
        return;
    }

    u8 value = 0;
    while (ps2_read_aux_byte(&value)) {
        mouse_process_byte(value);
    }
}
