#include "kernel.h"
#include "pci.h"
#include "port.h"
#include "string.h"

/*
 * Minimal xHCI (USB 3.x host) + HID boot mouse support.
 * Polled (no IDT/IRQs required), intended to work in QEMU with `-device qemu-xhci`.
 *
 * Notes:
 * - Assumes xHCI MMIO BAR is below 4GiB (your boot paging identity-maps first 4GiB).
 * - Handles a single attached HID boot mouse.
 */

/* MMIO helpers */
static inline u32 mmio_read32(volatile void *base, usize off) {
    volatile u32 *p = (volatile u32 *)((volatile u8 *)base + off);
    return *p;
}
static inline void mmio_write32(volatile void *base, usize off, u32 v) {
    volatile u32 *p = (volatile u32 *)((volatile u8 *)base + off);
    *p = v;
}
static inline u64 mmio_read64(volatile void *base, usize off) {
    volatile u64 *p = (volatile u64 *)((volatile u8 *)base + off);
    return *p;
}
static inline void mmio_write64(volatile void *base, usize off, u64 v) {
    volatile u64 *p = (volatile u64 *)((volatile u8 *)base + off);
    *p = v;
}

static void udelay(usize n) {
    for (volatile usize i = 0; i < n; i++) {
        __asm__ volatile ("" : : : "memory");
    }
}

static void *kmalloc_aligned(usize size, usize align) {
    /* Simple bump-align using over-allocation. */
    const usize extra = align ? (align - 1) : 0;
    u8 *raw = (u8 *)kmalloc(size + extra);
    if (!raw) {
        return 0;
    }
    usize p = (usize)raw;
    if (align) {
        p = (p + (align - 1)) & ~(align - 1);
    }
    return (void *)p;
}

static inline u64 phys64(const void *p) {
    /* Identity map for now. */
    return (u64)(usize)p;
}

typedef struct __attribute__((packed)) {
    u64 param;
    u32 status;
    u32 control;
} xhci_trb_t;

/* TRB types (control bits 10..15) */
enum {
    TRB_TYPE_NORMAL = 1,
    TRB_TYPE_SETUP_STAGE = 2,
    TRB_TYPE_DATA_STAGE = 3,
    TRB_TYPE_STATUS_STAGE = 4,
    TRB_TYPE_LINK = 6,

    TRB_TYPE_ENABLE_SLOT_CMD = 9,
    TRB_TYPE_ADDRESS_DEVICE_CMD = 11,
    TRB_TYPE_CONFIGURE_ENDPOINT_CMD = 12,
    TRB_TYPE_EVALUATE_CONTEXT_CMD = 13,

    TRB_TYPE_TRANSFER_EVENT = 32,
    TRB_TYPE_COMMAND_COMPLETION_EVENT = 33,
    TRB_TYPE_PORT_STATUS_CHANGE_EVENT = 34
};

static inline u32 trb_type(u32 t) { return (t & 0x3FU) << 10; }
static inline u32 trb_cycle(u32 c) { return c & 1U; }

typedef struct {
    xhci_trb_t *trbs;
    u32 size;     /* count */
    u32 enqueue;
    u32 cycle;
} trb_ring_t;

static void ring_init(trb_ring_t *r, xhci_trb_t *trbs, u32 count) {
    r->trbs = trbs;
    r->size = count;
    r->enqueue = 0;
    r->cycle = 1;
    kmemset(trbs, 0, (usize)count * sizeof(xhci_trb_t));

    /* Link TRB at end to wrap. */
    xhci_trb_t *link = &trbs[count - 1];
    link->param = phys64(trbs);
    link->status = 0;
    /* Toggle Cycle (TC=1 at bit1). */
    link->control = trb_cycle(r->cycle) | trb_type(TRB_TYPE_LINK) | (1U << 1);
}

static xhci_trb_t *ring_next(trb_ring_t *r) {
    xhci_trb_t *t = &r->trbs[r->enqueue];
    r->enqueue++;
    if (r->enqueue == r->size - 1) {
        /* advance over link TRB; toggle cycle */
        r->enqueue = 0;
        r->cycle ^= 1U;
        /* Keep the Link TRB cycle bit in sync with the current ring cycle. */
        xhci_trb_t *link = &r->trbs[r->size - 1];
        link->control = (link->control & ~1U) | (r->cycle & 1U);
    }
    return t;
}

static void ring_push(trb_ring_t *r, xhci_trb_t trb) {
    xhci_trb_t *dst = ring_next(r);
    trb.control &= ~1U;
    trb.control |= (r->cycle & 1U);
    *dst = trb;
}

/* xHCI registers */
static volatile u8 *g_mmio;
static volatile u8 *g_op;
static volatile u8 *g_db;
static volatile u8 *g_rt;

static u8 g_max_slots;
static u8 g_max_ports;
static bool g_ctx64;

static bool g_ready;
static u8 g_slot_id;

/* Event ring */
static xhci_trb_t *g_ev_trbs;
static u32 g_ev_size;
static u32 g_ev_deq;
static u32 g_ev_cycle;

/* Command ring */
static trb_ring_t g_cmd_ring;

/* DCBAA and contexts */
static u64 *g_dcbaa;
static void *g_input_ctx;
static void *g_output_ctx;
static trb_ring_t g_ep0_ring;
static trb_ring_t g_int_ring;

static u8 g_ep0_mps;
static bool g_mouse_ready;
static u8 g_mouse_ep;
static u16 g_mouse_mps;
static u8 g_mouse_iface;
static u8 g_mouse_interval;
static u8 g_int_toggle;
static u8 g_int_buf[8] __attribute__((aligned(16)));
static u8 g_last_buttons;

/* Runtime interrupter 0 offsets */
static usize rt_ir0(usize off) { return 0x20 + off; }

static void xhci_doorbell(u8 slot, u8 target) {
    mmio_write32((volatile void *)g_db, (usize)slot * 4, (u32)target);
}

static u32 op_read32(usize off) { return mmio_read32((volatile void *)g_op, off); }
static void op_write32(usize off, u32 v) { mmio_write32((volatile void *)g_op, off, v); }
static u32 rt_read32(usize off) { return mmio_read32((volatile void *)g_rt, off); }
static void rt_write32(usize off, u32 v) { mmio_write32((volatile void *)g_rt, off, v); }
static void rt_write64(usize off, u64 v) { mmio_write64((volatile void *)g_rt, off, v); }

static u8 port_speed_from_portsc(u32 portsc) {
    return (u8)((portsc >> 10) & 0x0F);
}

static u8 ep_id(u8 ep_num, bool in_dir) {
    if (ep_num == 0) {
        return in_dir ? 2 : 1;
    }
    return (u8)(2 * ep_num + (in_dir ? 2 : 1));
}

static u8 ctx_index_for_ep(u8 ep_num, bool in_dir) {
    /* Context index space: 0=slot, 1=ep0, 2=ep1out, 3=ep1in, ... */
    if (ep_num == 0) {
        return 1;
    }
    return (u8)(2 * ep_num + (in_dir ? 1 : 0));
}

static u32 make_portsc_reset(u32 portsc) {
    /* Write-1-to-clear bits: 17 CSC, 18 PEC, 20 PRC, 21 OCC, 22 WRC. */
    const u32 w1c = (1U << 17) | (1U << 18) | (1U << 20) | (1U << 21) | (1U << 22);
    portsc |= w1c;
    portsc |= (1U << 4); /* PR */
    return portsc;
}

static void *ctx_ptr(void *base, u32 index, u32 ctx_size) {
    return (void *)((u8 *)base + (usize)index * ctx_size);
}

static void xhci_setup_event_ring(void) {
    g_ev_size = 64;
    g_ev_trbs = (xhci_trb_t *)kmalloc_aligned((usize)g_ev_size * sizeof(xhci_trb_t), 64);
    kmemset(g_ev_trbs, 0, (usize)g_ev_size * sizeof(xhci_trb_t));
    g_ev_deq = 0;
    g_ev_cycle = 1;

    typedef struct __attribute__((packed)) {
        u64 addr;
        u32 size;
        u32 rsvd;
    } erst_entry_t;

    erst_entry_t *erst = (erst_entry_t *)kmalloc_aligned(sizeof(erst_entry_t), 64);
    erst->addr = phys64(g_ev_trbs);
    erst->size = g_ev_size;
    erst->rsvd = 0;

    /* Interrupter 0: ERSTSZ, ERSTBA, ERDP. */
    rt_write32(rt_ir0(0x08), 1);                 /* ERSTSZ */
    rt_write64(rt_ir0(0x10), phys64(erst));      /* ERSTBA */
    rt_write64(rt_ir0(0x18), phys64(g_ev_trbs)); /* ERDP */

    /* Enable interrupter (even if we poll) */
    rt_write32(rt_ir0(0x00), 0x00000002U); /* IMAN IE=1 */
}

static bool xhci_wait_event(u32 want_type, u8 *out_slot_id, u8 *out_cmpl_code, u32 *out_ptr_lo) {
    for (usize spins = 0; spins < 2000000; spins++) {
        xhci_trb_t *e = &g_ev_trbs[g_ev_deq];
        const u32 c = e->control & 1U;
        if (c != (g_ev_cycle & 1U)) {
            continue;
        }

        const u32 type = (e->control >> 10) & 0x3F;

        /* Advance dequeue */
        g_ev_deq++;
        if (g_ev_deq >= g_ev_size) {
            g_ev_deq = 0;
            g_ev_cycle ^= 1U;
        }
        rt_write64(rt_ir0(0x18), phys64(&g_ev_trbs[g_ev_deq]) | (1ULL << 3)); /* ERDP, EHB=1 */

        if (type != want_type) {
            continue;
        }

        if (out_slot_id) {
            *out_slot_id = (u8)((e->control >> 24) & 0xFF);
        }
        if (out_cmpl_code) {
            *out_cmpl_code = (u8)((e->status >> 24) & 0xFF);
        }
        if (out_ptr_lo) {
            *out_ptr_lo = (u32)(e->param & 0xFFFFFFFFULL);
        }
        return true;
    }
    return false;
}

static bool xhci_cmd_enable_slot(void) {
    xhci_trb_t trb;
    trb.param = 0;
    trb.status = 0;
    trb.control = trb_type(TRB_TYPE_ENABLE_SLOT_CMD);
    ring_push(&g_cmd_ring, trb);
    xhci_doorbell(0, 0);

    u8 slot = 0;
    u8 cc = 0;
    if (!xhci_wait_event(TRB_TYPE_COMMAND_COMPLETION_EVENT, &slot, &cc, 0)) {
        return false;
    }
    if (cc != 1 || slot == 0) { /* 1 = Success */
        return false;
    }
    g_slot_id = slot;
    return true;
}

static bool xhci_cmd_address_device(u8 port_id, u8 speed) {
    const u32 ctx_size = g_ctx64 ? 64U : 32U;
    const u32 icc_idx = 0;
    const u32 slot_idx = 1;
    const u32 ep0_idx = 2; /* endpoint 0 context is index 2 in xHCI context array */

    kmemset(g_input_ctx, 0, (usize)ctx_size * 33);
    kmemset(g_output_ctx, 0, (usize)ctx_size * 32);

    /* Input Control Context: add slot + ep0 contexts. */
    u32 *icc = (u32 *)ctx_ptr(g_input_ctx, icc_idx, ctx_size);
    icc[1] = (1U << 0) | (1U << 1); /* A0=slot, A1=ep0 */

    /* Slot Context */
    u32 *slot = (u32 *)ctx_ptr(g_input_ctx, slot_idx, ctx_size);
    /* DWORD0: Route String=0, Speed in bits 20..23 */
    slot[0] = ((u32)speed & 0x0F) << 20;
    /* DWORD1: Root Hub Port Number in bits 16..23 */
    slot[1] = ((u32)port_id & 0xFF) << 16;
    /* DWORD2: Context Entries in bits 27..31 (1 endpoint context: EP0) */
    slot[2] = (1U << 27);

    /* EP0 Context */
    u32 *ep0 = (u32 *)ctx_ptr(g_input_ctx, ep0_idx, ctx_size);
    /* EP Type = Control (4) in bits 3..5 of DWORD1. */
    ep0[1] = (4U << 3);
    /* Max Packet Size in bits 16..31 of DWORD1. Default 8 until Evaluate Context. */
    g_ep0_mps = 8;
    ep0[1] |= ((u32)g_ep0_mps) << 16;
    /* TR Dequeue Pointer in DWORD2/3, set DCS (bit0). */
    const u64 deq = phys64(g_ep0_ring.trbs) | (g_ep0_ring.cycle & 1U);
    ep0[2] = (u32)(deq & 0xFFFFFFFFULL);
    ep0[3] = (u32)(deq >> 32);
    /* Average TRB length (rough) */
    ep0[4] = 8;

    xhci_trb_t trb;
    trb.param = phys64(g_input_ctx);
    trb.status = 0;
    /* Slot ID in bits 24..31 */
    trb.control = trb_type(TRB_TYPE_ADDRESS_DEVICE_CMD) | ((u32)g_slot_id << 24);
    ring_push(&g_cmd_ring, trb);
    xhci_doorbell(0, 0);

    u8 slotid = 0;
    u8 cc = 0;
    if (!xhci_wait_event(TRB_TYPE_COMMAND_COMPLETION_EVENT, &slotid, &cc, 0)) {
        return false;
    }
    return (cc == 1) && (slotid == g_slot_id);
}

static bool xhci_cmd_evaluate_context(void) {
    xhci_trb_t trb;
    trb.param = phys64(g_input_ctx);
    trb.status = 0;
    trb.control = trb_type(TRB_TYPE_EVALUATE_CONTEXT_CMD) | ((u32)g_slot_id << 24);
    ring_push(&g_cmd_ring, trb);
    xhci_doorbell(0, 0);
    u8 slotid = 0;
    u8 cc = 0;
    if (!xhci_wait_event(TRB_TYPE_COMMAND_COMPLETION_EVENT, &slotid, &cc, 0)) {
        return false;
    }
    return (cc == 1) && (slotid == g_slot_id);
}

typedef struct __attribute__((packed)) {
    u8 bmRequestType;
    u8 bRequest;
    u16 wValue;
    u16 wIndex;
    u16 wLength;
} usb_setup_t;

static bool xhci_ctrl_in(const usb_setup_t *s, void *buf, u16 len) {
    /* Setup Stage TRB */
    xhci_trb_t t;
    t.param = ((u64)s->bmRequestType) |
              ((u64)s->bRequest << 8) |
              ((u64)s->wValue << 16) |
              ((u64)s->wIndex << 32) |
              ((u64)s->wLength << 48);
    t.status = 8;
    /* TRB Transfer Type: IN data stage (bit16..17 = 3 for IN) */
    t.control = trb_type(TRB_TYPE_SETUP_STAGE) | (3U << 16);
    ring_push(&g_ep0_ring, t);

    /* Data Stage TRB */
    xhci_trb_t d;
    d.param = phys64(buf);
    d.status = (u32)len;
    /* DIR=IN (bit16) */
    d.control = trb_type(TRB_TYPE_DATA_STAGE) | (1U << 16);
    ring_push(&g_ep0_ring, d);

    /* Status Stage TRB (OUT) */
    xhci_trb_t st;
    st.param = 0;
    st.status = 0;
    st.control = trb_type(TRB_TYPE_STATUS_STAGE) | (0U << 16);
    ring_push(&g_ep0_ring, st);

    xhci_doorbell(g_slot_id, ep_id(0, false));
    u8 cc = 0;
    if (!xhci_wait_event(TRB_TYPE_TRANSFER_EVENT, 0, &cc, 0)) {
        return false;
    }
    return cc == 1;
}

static bool xhci_ctrl_out(const usb_setup_t *s) {
    xhci_trb_t t;
    t.param = ((u64)s->bmRequestType) |
              ((u64)s->bRequest << 8) |
              ((u64)s->wValue << 16) |
              ((u64)s->wIndex << 32) |
              ((u64)s->wLength << 48);
    t.status = 8;
    /* No data stage */
    t.control = trb_type(TRB_TYPE_SETUP_STAGE) | (0U << 16);
    ring_push(&g_ep0_ring, t);

    xhci_trb_t st;
    st.param = 0;
    st.status = 0;
    /* Status stage is IN when no data stage on OUT request */
    st.control = trb_type(TRB_TYPE_STATUS_STAGE) | (1U << 16);
    ring_push(&g_ep0_ring, st);

    xhci_doorbell(g_slot_id, ep_id(0, false));
    u8 cc = 0;
    if (!xhci_wait_event(TRB_TYPE_TRANSFER_EVENT, 0, &cc, 0)) {
        return false;
    }
    return cc == 1;
}

static bool parse_hid_mouse_endpoint(const u8 *cfg, u16 cfg_len,
                                     u8 *out_cfg_value, u8 *out_iface, u8 *out_ep, u16 *out_maxpkt, u8 *out_interval) {
    if (!cfg || cfg_len < 9) {
        return false;
    }
    const u8 cfg_value = cfg[5];
    u8 cur_iface = 0xFF;
    u8 iface_class = 0;
    u8 iface_sub = 0;
    u8 iface_proto = 0;

    for (u16 i = 0; i + 2 < cfg_len; ) {
        const u8 len = cfg[i];
        const u8 type = cfg[i + 1];
        if (len == 0) {
            break;
        }
        if ((u16)(i + len) > cfg_len) {
            break;
        }
        if (type == 4 && len >= 9) {
            cur_iface = cfg[i + 2];
            iface_class = cfg[i + 5];
            iface_sub = cfg[i + 6];
            iface_proto = cfg[i + 7];
        } else if (type == 5 && len >= 7) {
            if (cur_iface != 0xFF &&
                iface_class == 0x03 &&
                iface_sub == 0x01 &&
                iface_proto == 0x02) {
                const u8 ep_addr = cfg[i + 2];
                const u8 attrs = cfg[i + 3];
                const u16 mps = (u16)cfg[i + 4] | ((u16)cfg[i + 5] << 8);
                const u8 interval = cfg[i + 6];
                const bool is_in = (ep_addr & 0x80) != 0;
                const u8 xfer_type = attrs & 0x03;
                if (is_in && xfer_type == 3 && mps > 0) {
                    *out_cfg_value = cfg_value;
                    *out_iface = cur_iface;
                    *out_ep = (u8)(ep_addr & 0x0F);
                    *out_maxpkt = mps;
                    *out_interval = interval;
                    return true;
                }
            }
        }
        i = (u16)(i + len);
    }
    return false;
}

static bool xhci_cmd_configure_interrupt_ep(u8 ep, u16 mps, u8 interval) {
    const u32 ctx_size = g_ctx64 ? 64U : 32U;
    const u32 icc_idx = 0;
    const u32 slot_idx = 1;
    const u32 ep_ctx_index = (u32)ctx_index_for_ep(ep, true);
    const u32 ep_array_idx = 1 + ep_ctx_index;

    kmemset(g_input_ctx, 0, (usize)ctx_size * 33);
    u32 *icc = (u32 *)ctx_ptr(g_input_ctx, icc_idx, ctx_size);
    icc[1] = (1U << ep_ctx_index); /* Add endpoint context */

    /* Slot context update: Context Entries >= interrupt ep */
    u32 *slot = (u32 *)ctx_ptr(g_input_ctx, slot_idx, ctx_size);
    const u8 ce = (u8)ep_ctx_index;
    slot[2] = ((u32)ce) << 27;

    /* Interrupt IN endpoint context */
    u32 *epc = (u32 *)ctx_ptr(g_input_ctx, ep_array_idx, ctx_size);
    /* EP Type: Interrupt IN = 7 (per spec encoding) in bits 3..5 */
    epc[1] = (7U << 3);
    epc[1] |= ((u32)mps) << 16;
    /* Interval in bits 16..23 of DWORD0 for interrupt endpoints (log2 based in xHCI),
       but devices report bInterval in frames. Keep it simple: clamp. */
    u8 iv = interval ? interval : 1;
    if (iv > 16) {
        iv = 16;
    }
    epc[0] |= ((u32)iv) << 16;

    const u64 deq = phys64(g_int_ring.trbs) | (g_int_ring.cycle & 1U);
    epc[2] = (u32)(deq & 0xFFFFFFFFULL);
    epc[3] = (u32)(deq >> 32);
    epc[4] = 4;

    xhci_trb_t trb;
    trb.param = phys64(g_input_ctx);
    trb.status = 0;
    trb.control = trb_type(TRB_TYPE_CONFIGURE_ENDPOINT_CMD) | ((u32)g_slot_id << 24);
    ring_push(&g_cmd_ring, trb);
    xhci_doorbell(0, 0);

    u8 slotid = 0;
    u8 cc = 0;
    if (!xhci_wait_event(TRB_TYPE_COMMAND_COMPLETION_EVENT, &slotid, &cc, 0)) {
        return false;
    }
    return (cc == 1) && (slotid == g_slot_id);
}

static void xhci_queue_interrupt_in(void) {
    xhci_trb_t n;
    n.param = phys64(g_int_buf);
    n.status = 3;
    /* IOC=1 (bit5) */
    n.control = trb_type(TRB_TYPE_NORMAL) | (1U << 5);
    ring_push(&g_int_ring, n);
    xhci_doorbell(g_slot_id, ep_id(g_mouse_ep, true));
}

bool xhci_usb_ready(void) {
    return g_ready && g_mouse_ready;
}

void xhci_usb_init(void) {
    g_ready = false;
    g_mouse_ready = false;
    g_slot_id = 0;
    g_mmio = 0;
    g_op = 0;
    g_db = 0;
    g_rt = 0;
    g_ep0_mps = 8;
    g_last_buttons = 0;

    pci_init();

    pci_addr_t xhci;
    if (!pci_find_class(0x0C, 0x03, 0x30, &xhci)) {
        log_write("usb(xhci): no controller found");
        return;
    }

    /* Enable bus mastering + memory space. */
    u16 cmd = pci_read_command(xhci);
    cmd |= 0x0002; /* MEM */
    cmd |= 0x0004; /* BUS MASTER */
    pci_write_command(xhci, cmd);

    const u32 bar0 = pci_read_bar(xhci, 0);
    const u32 bar1 = pci_read_bar(xhci, 1);
    u64 mmio = 0;
    if ((bar0 & 0x01) != 0) {
        log_write("usb(xhci): BAR0 is I/O (unexpected)");
        return;
    }
    const u32 type = (bar0 >> 1) & 0x3;
    if (type == 0x2) {
        mmio = ((u64)bar1 << 32) | (u64)(bar0 & 0xFFFFFFF0U);
    } else {
        mmio = (u64)(bar0 & 0xFFFFFFF0U);
    }
    if (mmio == 0 || mmio >= 0x100000000ULL) {
        log_write("usb(xhci): MMIO BAR not mapped (<4GiB required)");
        return;
    }

    g_mmio = (volatile u8 *)(usize)mmio;
    const u8 caplen = *(volatile u8 *)(g_mmio + 0x00);
    const u32 hcs1 = mmio_read32((volatile void *)g_mmio, 0x04);
    const u32 hcc1 = mmio_read32((volatile void *)g_mmio, 0x10);
    const u32 dboff = mmio_read32((volatile void *)g_mmio, 0x14);
    const u32 rtsoff = mmio_read32((volatile void *)g_mmio, 0x18);

    g_op = g_mmio + caplen;
    g_db = g_mmio + (dboff & ~0x3U);
    g_rt = g_mmio + (rtsoff & ~0x1FU);

    g_max_slots = (u8)(hcs1 & 0xFF);
    g_max_ports = (u8)((hcs1 >> 24) & 0xFF);
    g_ctx64 = ((hcc1 >> 2) & 1U) != 0;
    if (g_max_slots == 0 || g_max_ports == 0) {
        log_write("usb(xhci): bad HCSPARAMS");
        return;
    }

    /* Halt + reset controller */
    op_write32(0x00, op_read32(0x00) & ~1U); /* USBCMD.RS=0 */
    for (usize i = 0; i < 200000; i++) {
        if (op_read32(0x04) & 1U) { /* USBSTS.HCH */
            break;
        }
    }
    op_write32(0x00, op_read32(0x00) | (1U << 1)); /* USBCMD.HCRST */
    for (usize i = 0; i < 500000; i++) {
        if ((op_read32(0x00) & (1U << 1)) == 0) {
            break;
        }
    }

    /* Allocate structures */
    const u32 ctx_size = g_ctx64 ? 64U : 32U;
    g_dcbaa = (u64 *)kmalloc_aligned((usize)(g_max_slots + 1) * sizeof(u64), 64);
    kmemset(g_dcbaa, 0, (usize)(g_max_slots + 1) * sizeof(u64));

    g_output_ctx = kmalloc_aligned((usize)ctx_size * 32, 64);
    g_input_ctx = kmalloc_aligned((usize)ctx_size * 33, 64);

    /* Command ring */
    xhci_trb_t *cmd_trbs = (xhci_trb_t *)kmalloc_aligned((usize)64 * sizeof(xhci_trb_t), 64);
    ring_init(&g_cmd_ring, cmd_trbs, 64);

    /* EP0 transfer ring */
    xhci_trb_t *ep0_trbs = (xhci_trb_t *)kmalloc_aligned((usize)64 * sizeof(xhci_trb_t), 64);
    ring_init(&g_ep0_ring, ep0_trbs, 64);

    /* Interrupt ring */
    xhci_trb_t *int_trbs = (xhci_trb_t *)kmalloc_aligned((usize)64 * sizeof(xhci_trb_t), 64);
    ring_init(&g_int_ring, int_trbs, 64);

    xhci_setup_event_ring();

    /* Program DCBAAP and CRCR */
    mmio_write64((volatile void *)g_op, 0x30, phys64(g_dcbaa)); /* DCBAAP */
    mmio_write64((volatile void *)g_op, 0x18, phys64(g_cmd_ring.trbs) | (g_cmd_ring.cycle & 1U)); /* CRCR */
    op_write32(0x38, g_max_slots); /* CONFIG */

    /* Run */
    op_write32(0x00, op_read32(0x00) | 1U);

    /* Find a connected port and reset it */
    u8 port = 0;
    u8 speed = 0;
    for (u8 i = 0; i < g_max_ports; i++) {
        const u32 portsc = op_read32(0x400 + (usize)i * 0x10);
        if (portsc & 1U) {
            port = (u8)(i + 1);
            speed = port_speed_from_portsc(portsc);
            /* Port reset */
            op_write32(0x400 + (usize)i * 0x10, make_portsc_reset(portsc));
            for (usize w = 0; w < 500000; w++) {
                const u32 ps = op_read32(0x400 + (usize)i * 0x10);
                if ((ps & (1U << 4)) == 0) {
                    break;
                }
            }
            udelay(200000);
            break;
        }
    }
    if (port == 0) {
        log_write("usb(xhci): no device connected");
        return;
    }

    if (!xhci_cmd_enable_slot()) {
        log_write("usb(xhci): enable slot failed");
        return;
    }

    /* Output device context for slot */
    g_dcbaa[g_slot_id] = phys64(g_output_ctx);

    if (!xhci_cmd_address_device(port, speed)) {
        log_write("usb(xhci): address device failed");
        return;
    }

    /* Read device descriptor (first 18 bytes) */
    u8 dev_desc[18];
    usb_setup_t getdd;
    getdd.bmRequestType = 0x80;
    getdd.bRequest = 6;
    getdd.wValue = 0x0100;
    getdd.wIndex = 0;
    getdd.wLength = 18;
    if (!xhci_ctrl_in(&getdd, dev_desc, 18)) {
        log_write("usb(xhci): get device desc failed");
        return;
    }
    const u8 mps0 = dev_desc[7];
    if (mps0 != 0 && mps0 != g_ep0_mps) {
        /* Update EP0 MPS via Evaluate Context */
        const u32 ctx_size2 = g_ctx64 ? 64U : 32U;
        kmemset(g_input_ctx, 0, (usize)ctx_size2 * 33);
        u32 *icc = (u32 *)ctx_ptr(g_input_ctx, 0, ctx_size2);
        icc[1] = (1U << 1); /* A1 ep0 */
        u32 *ep0 = (u32 *)ctx_ptr(g_input_ctx, 2, ctx_size2);
        ep0[1] = (4U << 3) | ((u32)mps0 << 16);
        const u64 deq = phys64(g_ep0_ring.trbs) | (g_ep0_ring.cycle & 1U);
        ep0[2] = (u32)(deq & 0xFFFFFFFFULL);
        ep0[3] = (u32)(deq >> 32);
        ep0[4] = 8;
        if (xhci_cmd_evaluate_context()) {
            g_ep0_mps = mps0;
        }
    }

    /* Get config descriptor header */
    u8 cfg_hdr[9];
    usb_setup_t getcfg;
    getcfg.bmRequestType = 0x80;
    getcfg.bRequest = 6;
    getcfg.wValue = 0x0200;
    getcfg.wIndex = 0;
    getcfg.wLength = 9;
    if (!xhci_ctrl_in(&getcfg, cfg_hdr, 9)) {
        log_write("usb(xhci): get cfg hdr failed");
        return;
    }
    const u16 total_len = (u16)cfg_hdr[2] | ((u16)cfg_hdr[3] << 8);
    if (total_len < 9 || total_len > 256) {
        log_write("usb(xhci): cfg len invalid");
        return;
    }
    u8 cfg_all[256];
    getcfg.wLength = total_len;
    if (!xhci_ctrl_in(&getcfg, cfg_all, total_len)) {
        log_write("usb(xhci): get cfg failed");
        return;
    }

    u8 cfg_value = 0;
    u8 iface = 0;
    u8 ep = 0;
    u16 mps = 0;
    u8 interval = 1;
    if (!parse_hid_mouse_endpoint(cfg_all, total_len, &cfg_value, &iface, &ep, &mps, &interval)) {
        log_write("usb(xhci): no HID boot mouse iface");
        return;
    }

    /* Set configuration */
    usb_setup_t setcfg;
    setcfg.bmRequestType = 0x00;
    setcfg.bRequest = 9;
    setcfg.wValue = cfg_value;
    setcfg.wIndex = 0;
    setcfg.wLength = 0;
    (void)xhci_ctrl_out(&setcfg);

    /* HID: SET_PROTOCOL boot, SET_IDLE */
    usb_setup_t setproto;
    setproto.bmRequestType = 0x21;
    setproto.bRequest = 0x0B;
    setproto.wValue = 0;
    setproto.wIndex = iface;
    setproto.wLength = 0;
    (void)xhci_ctrl_out(&setproto);

    usb_setup_t setidle;
    setidle.bmRequestType = 0x21;
    setidle.bRequest = 0x0A;
    setidle.wValue = 0;
    setidle.wIndex = iface;
    setidle.wLength = 0;
    (void)xhci_ctrl_out(&setidle);

    g_mouse_ep = ep;
    g_mouse_mps = mps;
    g_mouse_iface = iface;
    g_mouse_interval = interval;
    g_int_toggle = 1;

    if (!xhci_cmd_configure_interrupt_ep(ep, mps, interval)) {
        log_write("usb(xhci): configure ep failed");
        return;
    }

    /* Queue first interrupt transfer */
    xhci_queue_interrupt_in();

    g_mouse_ready = true;
    g_ready = true;
    log_write("usb(xhci): HID mouse ready");
}

void xhci_usb_poll(void) {
    if (!g_ready || !g_mouse_ready) {
        return;
    }

    /* Poll for a transfer event completion by watching the event ring. */
    xhci_trb_t *e = &g_ev_trbs[g_ev_deq];
    const u32 c = e->control & 1U;
    if (c != (g_ev_cycle & 1U)) {
        return;
    }
    const u32 type = (e->control >> 10) & 0x3F;
    if (type != TRB_TYPE_TRANSFER_EVENT) {
        /* Consume and drop unrelated events. */
        g_ev_deq++;
        if (g_ev_deq >= g_ev_size) {
            g_ev_deq = 0;
            g_ev_cycle ^= 1U;
        }
        rt_write64(rt_ir0(0x18), phys64(&g_ev_trbs[g_ev_deq]) | (1ULL << 3));
        return;
    }

    const u8 cc = (u8)((e->status >> 24) & 0xFF);
    /* Consume event */
    g_ev_deq++;
    if (g_ev_deq >= g_ev_size) {
        g_ev_deq = 0;
        g_ev_cycle ^= 1U;
    }
    rt_write64(rt_ir0(0x18), phys64(&g_ev_trbs[g_ev_deq]) | (1ULL << 3));

    if (cc != 1) {
        /* Requeue anyway. */
        xhci_queue_interrupt_in();
        return;
    }

    const u8 buttons = g_int_buf[0];
    const i8 dx = (i8)g_int_buf[1];
    const i8 dy = (i8)g_int_buf[2];

    if (dx != 0 || dy != 0) {
        input_event_t move_evt;
        move_evt.type = INPUT_EVENT_MOUSE_MOVE;
        move_evt.a = (i32)dx;
        move_evt.b = (i32)(-dy);
        move_evt.c = 0;
        input_event_push(move_evt);
    }

    if (buttons != g_last_buttons) {
        input_event_t btn_evt;
        btn_evt.type = INPUT_EVENT_MOUSE_BUTTON;
        btn_evt.a = (buttons & 0x01) ? 1 : 0;
        btn_evt.b = (buttons & 0x02) ? 1 : 0;
        btn_evt.c = (buttons & 0x04) ? 1 : 0;
        input_event_push(btn_evt);
        g_last_buttons = buttons;
    }

    /* Queue next */
    xhci_queue_interrupt_in();
}
