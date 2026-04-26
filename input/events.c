#include "kernel.h"
#include "string.h"
#include "port.h"

#define INPUT_QUEUE_CAPACITY 256
#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64

static input_event_t g_events[INPUT_QUEUE_CAPACITY];
static usize g_head;
static usize g_tail;
static usize g_count;

bool input_event_push(input_event_t event) {
    if (g_count >= INPUT_QUEUE_CAPACITY) {
        return false;
    }

    g_events[g_tail] = event;
    g_tail = (g_tail + 1) % INPUT_QUEUE_CAPACITY;
    g_count++;
    return true;
}

bool input_event_pop(input_event_t *event) {
    if (!event || g_count == 0) {
        return false;
    }

    *event = g_events[g_head];
    g_head = (g_head + 1) % INPUT_QUEUE_CAPACITY;
    g_count--;
    return true;
}

void input_events_init(void) {
    g_head = 0;
    g_tail = 0;
    g_count = 0;
    kmemset(g_events, 0, sizeof(g_events));
    log_write("input: unified event queue initialized");
}

static void i8042_poll(void) {
    extern void keyboard_handle_byte(u8 sc);
    extern void mouse_handle_byte(u8 b);
    extern bool mouse_mid_packet(void);

    for (;;) {
        const u8 status = inb(PS2_STATUS_PORT);
        if ((status & 0x01) == 0) {
            break;
        }

        const u8 data = inb(PS2_DATA_PORT);

        /* Prefer controller AUX bit (mouse) when it's present. */
        if ((status & 0x20) != 0) {
            mouse_handle_byte(data);
            continue;
        }

        /* Conservative fallback: only continue an already-started mouse packet.
           Starting a packet based on heuristics causes cursor "teleporting" when
           keyboard bytes get misclassified as mouse bytes. */
        if (mouse_mid_packet()) {
            mouse_handle_byte(data);
            continue;
        }

        keyboard_handle_byte(data);
    }
}

void input_poll(void) {
    i8042_poll();
    usb_poll();
}
