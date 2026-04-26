#ifndef NET_H
#define NET_H

#include "types.h"

typedef struct {
    u8 mac[6];
    u32 ip;       /* host order */
    u32 netmask;  /* host order */
    u32 gw;       /* host order */
    bool up;
} net_state_t;

void net_init(void);
void net_poll(void);
const net_state_t *net_state(void);
bool net_connect(void);
void net_disconnect(void);

/* Minimal HTTP (no TLS). IP is host-order IPv4. Returns bytes written to out. */
usize net_http_get(u32 ip, const char *path, char *out, usize out_cap);

#endif
