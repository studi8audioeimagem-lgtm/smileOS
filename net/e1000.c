#include "kernel.h"
#include "pci.h"
#include "port.h"
#include "string.h"
#include "types.h"
#include "e1000.h"

/* Minimal E1000 driver (I/O mapped register access). Works with QEMU e1000 (82540EM, 0x8086:0x100E). */

enum {
    E1000_REG_CTRL   = 0x0000,
    E1000_REG_STATUS = 0x0008,
    E1000_REG_EECD   = 0x0010,
    E1000_REG_IMS    = 0x00D0,
    E1000_REG_RCTL   = 0x0100,
    E1000_REG_TCTL   = 0x0400,
    E1000_REG_TIPG   = 0x0410,
    E1000_REG_RDBAL  = 0x2800,
    E1000_REG_RDBAH  = 0x2804,
    E1000_REG_RDLEN  = 0x2808,
    E1000_REG_RDH    = 0x2810,
    E1000_REG_RDT    = 0x2818,
    E1000_REG_TDBAL  = 0x3800,
    E1000_REG_TDBAH  = 0x3804,
    E1000_REG_TDLEN  = 0x3808,
    E1000_REG_TDH    = 0x3810,
    E1000_REG_TDT    = 0x3818,
    E1000_REG_RAL    = 0x5400,
    E1000_REG_RAH    = 0x5404,
};

enum {
    CTRL_RST = (1U << 26),
};

enum {
    RCTL_EN    = (1U << 1),
    RCTL_BAM   = (1U << 15),
    RCTL_SECRC = (1U << 26),
};

enum {
    TCTL_EN  = (1U << 1),
    TCTL_PSP = (1U << 3),
};

typedef struct {
    u64 addr;
    u16 length;
    u16 csum;
    u8 status;
    u8 errors;
    u16 special;
} __attribute__((packed)) rx_desc_t;

typedef struct {
    u64 addr;
    u16 length;
    u8 cso;
    u8 cmd;
    u8 status;
    u8 css;
    u16 special;
} __attribute__((packed)) tx_desc_t;

enum { RX_COUNT = 32, TX_COUNT = 32 };

static bool g_ready;
static u16 g_iobase;
static rx_desc_t *g_rx;
static tx_desc_t *g_tx;
static u8 *g_rx_bufs[RX_COUNT];
static u8 *g_tx_bufs[TX_COUNT];
static u32 g_rx_cur;
static u32 g_tx_cur;
static u8 g_mac[6];

static inline void e1000_io_write(u32 reg, u32 value) {
    outl(g_iobase + 0, reg);
    outl(g_iobase + 4, value);
}

static inline u32 e1000_io_read(u32 reg) {
    outl(g_iobase + 0, reg);
    return inl(g_iobase + 4);
}

static void e1000_delay(void) {
    for (volatile u32 i = 0; i < 20000U; i++) {
        __asm__ volatile ("pause");
    }
}

static void e1000_read_mac(void) {
    const u32 ral = e1000_io_read(E1000_REG_RAL);
    const u32 rah = e1000_io_read(E1000_REG_RAH);
    g_mac[0] = (u8)(ral & 0xFF);
    g_mac[1] = (u8)((ral >> 8) & 0xFF);
    g_mac[2] = (u8)((ral >> 16) & 0xFF);
    g_mac[3] = (u8)((ral >> 24) & 0xFF);
    g_mac[4] = (u8)(rah & 0xFF);
    g_mac[5] = (u8)((rah >> 8) & 0xFF);
}

bool e1000_init(void) {
    g_ready = false;
    g_iobase = 0;
    g_rx = 0;
    g_tx = 0;
    g_rx_cur = 0;
    g_tx_cur = 0;
    kmemset(g_mac, 0, sizeof(g_mac));

    /* Find QEMU e1000 (82540EM). */
    pci_addr_t dev;
    if (!pci_find_device(0x8086, 0x100E, &dev)) {
        log_write("net(e1000): device not found");
        return false;
    }

    /* Enable I/O space + bus mastering. */
    u16 cmd = pci_read_command(dev);
    cmd |= (1U << 0); /* I/O space */
    cmd |= (1U << 2); /* bus master */
    pci_write_command(dev, cmd);

    const u32 bar1 = pci_read_bar(dev, 1);
    if ((bar1 & 1U) == 0) {
        log_write("net(e1000): BAR1 not I/O");
        return false;
    }
    g_iobase = (u16)(bar1 & 0xFFF8U);

    /* Reset */
    e1000_io_write(E1000_REG_CTRL, e1000_io_read(E1000_REG_CTRL) | CTRL_RST);
    e1000_delay();

    /* Mask interrupts; we'll poll. */
    e1000_io_write(E1000_REG_IMS, 0);

    /* Allocate rings + buffers. */
    g_rx = (rx_desc_t *)kmalloc(sizeof(rx_desc_t) * RX_COUNT);
    g_tx = (tx_desc_t *)kmalloc(sizeof(tx_desc_t) * TX_COUNT);
    if (!g_rx || !g_tx) {
        log_write("net(e1000): oom rings");
        return false;
    }
    kmemset(g_rx, 0, sizeof(rx_desc_t) * RX_COUNT);
    kmemset(g_tx, 0, sizeof(tx_desc_t) * TX_COUNT);

    for (u32 i = 0; i < RX_COUNT; i++) {
        g_rx_bufs[i] = (u8 *)kmalloc(2048);
        if (!g_rx_bufs[i]) {
            log_write("net(e1000): oom rx buf");
            return false;
        }
        kmemset(g_rx_bufs[i], 0, 2048);
        g_rx[i].addr = (u64)(usize)g_rx_bufs[i];
        g_rx[i].status = 0;
    }
    for (u32 i = 0; i < TX_COUNT; i++) {
        g_tx_bufs[i] = (u8 *)kmalloc(2048);
        if (!g_tx_bufs[i]) {
            log_write("net(e1000): oom tx buf");
            return false;
        }
        kmemset(g_tx_bufs[i], 0, 2048);
        g_tx[i].addr = (u64)(usize)g_tx_bufs[i];
        g_tx[i].status = 0x1; /* DD set initially */
    }

    /* Program RX ring */
    const u64 rx_pa = (u64)(usize)g_rx;
    e1000_io_write(E1000_REG_RDBAL, (u32)(rx_pa & 0xFFFFFFFFU));
    e1000_io_write(E1000_REG_RDBAH, (u32)(rx_pa >> 32));
    e1000_io_write(E1000_REG_RDLEN, (u32)(sizeof(rx_desc_t) * RX_COUNT));
    e1000_io_write(E1000_REG_RDH, 0);
    e1000_io_write(E1000_REG_RDT, RX_COUNT - 1);

    /* Program TX ring */
    const u64 tx_pa = (u64)(usize)g_tx;
    e1000_io_write(E1000_REG_TDBAL, (u32)(tx_pa & 0xFFFFFFFFU));
    e1000_io_write(E1000_REG_TDBAH, (u32)(tx_pa >> 32));
    e1000_io_write(E1000_REG_TDLEN, (u32)(sizeof(tx_desc_t) * TX_COUNT));
    e1000_io_write(E1000_REG_TDH, 0);
    e1000_io_write(E1000_REG_TDT, 0);

    /* Enable RX/TX */
    e1000_io_write(E1000_REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);
    e1000_io_write(E1000_REG_TCTL, TCTL_EN | TCTL_PSP | (0x10U << 4) | (0x40U << 12));
    e1000_io_write(E1000_REG_TIPG, 0x0060200A);

    e1000_read_mac();
    g_ready = true;
    log_write("net(e1000): initialized");
    return true;
}

bool e1000_link_up(void) {
    if (!g_ready) return false;
    const u32 st = e1000_io_read(E1000_REG_STATUS);
    return (st & (1U << 1)) != 0; /* LU */
}

void e1000_get_mac(u8 out_mac[6]) {
    if (!out_mac) return;
    for (u32 i = 0; i < 6; i++) out_mac[i] = g_mac[i];
}

bool e1000_send(const void *frame, u16 len) {
    if (!g_ready || !frame || len == 0 || len > 1514) {
        return false;
    }

    tx_desc_t *d = &g_tx[g_tx_cur];
    if ((d->status & 0x1) == 0) {
        return false; /* busy */
    }

    kmemcpy(g_tx_bufs[g_tx_cur], frame, len);

    d->length = len;
    d->cso = 0;
    d->cmd = (1U << 0) | (1U << 1) | (1U << 3); /* EOP | IFCS | RS */
    d->status = 0;

    const u32 cur = g_tx_cur;
    g_tx_cur = (g_tx_cur + 1) % TX_COUNT;
    e1000_io_write(E1000_REG_TDT, g_tx_cur);

    /* wait a bit for completion (poll) */
    for (u32 i = 0; i < 200000U; i++) {
        if (g_tx[cur].status & 0x1) {
            return true;
        }
        __asm__ volatile ("pause");
    }
    return false;
}

u16 e1000_recv(void *out_frame, u16 cap) {
    if (!g_ready || !out_frame || cap < 64) {
        return 0;
    }

    rx_desc_t *d = &g_rx[g_rx_cur];
    if ((d->status & 0x1) == 0) {
        return 0;
    }

    u16 len = d->length;
    if (len > cap) len = cap;
    kmemcpy(out_frame, g_rx_bufs[g_rx_cur], len);

    d->status = 0;
    /* Update RDT to hand this descriptor back. */
    e1000_io_write(E1000_REG_RDT, g_rx_cur);
    g_rx_cur = (g_rx_cur + 1) % RX_COUNT;
    return len;
}

