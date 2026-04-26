#ifndef I8042_H
#define I8042_H

#include "types.h"

bool i8042_wait_input_clear(void);
bool i8042_wait_output_full(void);
void i8042_drain_output(void);
void i8042_write_cmd(u8 cmd);
void i8042_write_data(u8 data);
u8 i8042_read_data(void);
u8 i8042_read_command_byte(void);
void i8042_write_command_byte(u8 value);

#endif
