#ifndef E1000_H
#define E1000_H

#include "types.h"

bool e1000_init(void);
bool e1000_link_up(void);
void e1000_get_mac(u8 out_mac[6]);
bool e1000_send(const void *frame, u16 len);
u16 e1000_recv(void *out_frame, u16 cap); /* returns length, 0 if none */

#endif

