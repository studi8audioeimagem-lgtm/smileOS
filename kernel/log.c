#include "kernel.h"
#include "types.h"
#include "string.h"
#include "port.h"

#define LOG_BUFFER_SIZE 4096

static char g_log_buffer[LOG_BUFFER_SIZE];
static usize g_log_head;
static bool g_serial_ready;

static void serial_init(void) {
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x03);
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
    outb(0x3F8 + 4, 0x0B);
    g_serial_ready = true;
}

static void serial_write_char(char c) {
    if (!g_serial_ready) {
        return;
    }

    while ((inb(0x3F8 + 5) & 0x20) == 0) { }
    outb(0x3F8, (u8)c);
}

void log_init(void) {
    g_log_head = 0;
    g_serial_ready = false;
    kmemset(g_log_buffer, 0, sizeof(g_log_buffer));
    serial_init();
}

void log_write(const char *message) {
    if (!message) {
        return;
    }

    for (usize i = 0; message[i] != 0; i++) {
        g_log_buffer[g_log_head] = message[i];
        g_log_head = (g_log_head + 1) % LOG_BUFFER_SIZE;
    }

    g_log_buffer[g_log_head] = '\n';
    g_log_head = (g_log_head + 1) % LOG_BUFFER_SIZE;

    for (usize i = 0; message[i] != 0; i++) {
        serial_write_char(message[i]);
    }
    serial_write_char('\n');
}
