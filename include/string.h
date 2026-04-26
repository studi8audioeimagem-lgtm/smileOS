#ifndef STRING_H
#define STRING_H

#include "types.h"

static inline usize kstrlen(const char *s) {
    usize len = 0;
    while (s && s[len]) { len++; }
    return len;
}

static inline void *kmemset(void *dest, int value, usize count) {
    /* Fast freestanding memset using REP STOSB (works fine at -O0 too). */
    void *ret = dest;
    u8 v = (u8)value;
    __asm__ volatile("cld; rep stosb"
                     : "+D"(dest), "+c"(count)
                     : "a"(v)
                     : "memory");
    return ret;
}

static inline void *kmemcpy(void *dest, const void *src, usize count) {
    /* Fast freestanding memcpy using REP MOVSB (works fine at -O0 too). */
    void *ret = dest;
    __asm__ volatile("cld; rep movsb"
                     : "+D"(dest), "+S"(src), "+c"(count)
                     :
                     : "memory");
    return ret;
}

#endif
