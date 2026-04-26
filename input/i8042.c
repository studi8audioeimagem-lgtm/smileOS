#include "i8042.h"
#include "port.h"

#define I8042_DATA_PORT 0x60
#define I8042_STATUS_PORT 0x64
#define I8042_CMD_PORT 0x64

bool i8042_wait_input_clear(void) {
    for (usize i = 0; i < 200000; i++) {
        if ((inb(I8042_STATUS_PORT) & 0x02) == 0) {
            return true;
        }
    }
    return false;
}

bool i8042_wait_output_full(void) {
    for (usize i = 0; i < 200000; i++) {
        if ((inb(I8042_STATUS_PORT) & 0x01) != 0) {
            return true;
        }
    }
    return false;
}

void i8042_drain_output(void) {
    for (usize i = 0; i < 64; i++) {
        if ((inb(I8042_STATUS_PORT) & 0x01) == 0) {
            break;
        }
        (void)inb(I8042_DATA_PORT);
    }
}

void i8042_write_cmd(u8 cmd) {
    if (!i8042_wait_input_clear()) {
        return;
    }
    outb(I8042_CMD_PORT, cmd);
}

void i8042_write_data(u8 data) {
    if (!i8042_wait_input_clear()) {
        return;
    }
    outb(I8042_DATA_PORT, data);
}

u8 i8042_read_data(void) {
    if (!i8042_wait_output_full()) {
        return 0;
    }
    return inb(I8042_DATA_PORT);
}

u8 i8042_read_command_byte(void) {
    i8042_drain_output();
    i8042_write_cmd(0x20);
    return i8042_read_data();
}

void i8042_write_command_byte(u8 value) {
    i8042_drain_output();
    i8042_write_cmd(0x60);
    i8042_write_data(value);
}

