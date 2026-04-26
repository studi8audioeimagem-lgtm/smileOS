#include "kernel.h"
#include "string.h"

/* 1 MiB was too small once we enabled double-buffering and richer UI.
   This is still a simple bump allocator, so "leaks" are expected for now. */
#define HEAP_SIZE (16 * 1024 * 1024)

static unsigned char g_heap[HEAP_SIZE];
static usize g_heap_offset;

void heap_init(void) {
    g_heap_offset = 0;
    kmemset(g_heap, 0, sizeof(g_heap));
}

void *kmalloc(usize size) {
    if (size == 0 || g_heap_offset + size > HEAP_SIZE) {
        return 0;
    }

    void *ptr = &g_heap[g_heap_offset];
    g_heap_offset += (size + 7) & ~((usize)7);
    return ptr;
}

void kfree(void *ptr) {
    (void)ptr;
}
