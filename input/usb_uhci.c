#include "kernel.h"
#include "pci.h"
#include "port.h"
#include "string.h"

/*
 * Minimal UHCI (USB 1.1) + HID boot mouse support, designed for QEMU.
 * - polled (no IRQ/IDT needed)
 * - supports a single device on root port 1 or 2
 * - supports low-speed HID boot mouse (3-byte report)
 */

/* UHCI I/O registers (word unless noted). */
#define UHCI_USBCMD      0x00 /* w */
#define UHCI_USBSTS      0x02 /* w */
#define UHCI_USBINTR     0x04 /* w */
#define UHCI_FRNUM       0x06 /* w */
#define UHCI_FRBASEADD   0x08 /* d */
#define UHCI_SOFMOD      0x0C /* b */
#define UHCI_PORTSC1     0x10 /* w */
#define UHCI_PORTSC2     0x12 /* w */

/* USBCMD bits */
#define USBCMD_RUN       0x0001
#define USBCMD_HCRESET   0x0002
#define USBCMD_GRESET    0x0004
#define USBCMD_MAXP      0x0080

/* USBSTS bits */
#define USBSTS_USBINT    0x0001
#define USBSTS_ERROR     0x0002
#define USBSTS_HSE       0x0004
#define USBSTS_HCPE      0x0010
#define USBSTS_HCHALTED  0x0020

/* PORTSC bits (UHCI legacy) */
#define PORTSC_CCS       0x0001
#define PORTSC_CSC       0x0002
#define PORTSC_PED       0x0004
#define PORTSC_PEC       0x0008
#define PORTSC_RESUME    0x0040
#define PORTSC_LSDA      0x0100
#define PORTSC_PR        0x0200

/* TD/QH link pointer bits */
#define UHCI_LINK_TERMINATE 0x00000001U
#define UHCI_LINK_QH        0x00000002U
#define UHCI_LINK_TD        0x00000000U

/* TD control/status bits */
#define TD_CTRL_ACTIVE      (1U << 23)
#define TD_CTRL_IOC         (1U << 24)
#define TD_CTRL_ISO         (1U << 25)
#define TD_CTRL_LS          (1U << 26)
#define TD_CTRL_ERRCNT_3    (3U << 27)
#define TD_CTRL_SPD         (1U << 29)

/* PID tokens */
#define USB_PID_OUT 0xE1
#define USB_PID_IN  0x69
#define USB_PID_SETUP 0x2D

typedef struct __attribute__((packed, aligned(16))) {
    u32 link_ptr;
    u32 ctrl_status;
    u32 token;
    u32 buffer;
} uhci_td_t;

typedef struct __attribute__((packed, aligned(16))) {
    u32 head_link_ptr;
    u32 element_link_ptr;
} uhci_qh_t;

static bool g_usb_ready;
static u16 g_io_base;
static bool g_dev_low_speed;
static u8 g_dev_addr;
static u8 g_ep0_maxpkt;

static u8 g_mouse_ep;        /* endpoint number */
static u16 g_mouse_maxpkt;
static u8 g_mouse_iface;
static bool g_mouse_ready;
static u8 g_int_toggle;

/* Frame list must be 4K aligned and contain 1024 entries. */
static u32 g_frame_list[1024] __attribute__((aligned(4096)));
static uhci_qh_t g_async_qh __attribute__((aligned(16)));

/* Reusable TDs/buffers for control and interrupt transfers. */
static uhci_td_t g_td_setup __attribute__((aligned(16)));
static uhci_td_t g_td_status __attribute__((aligned(16)));
static uhci_td_t g_td_data[32] __attribute__((aligned(16)));
static u8 g_ctrl_buf[256] __attribute__((aligned(16)));

static uhci_qh_t g_int_qh __attribute__((aligned(16)));
static uhci_td_t g_int_td __attribute__((aligned(16)));
static u8 g_int_buf[8] __attribute__((aligned(16)));
static u8 g_last_report[8];

static inline void io_writew(u16 reg, u16 v) { outw((u16)(g_io_base + reg), v); }
static inline u16 io_readw(u16 reg) { return inw((u16)(g_io_base + reg)); }
static inline void io_writel(u16 reg, u32 v) { outl((u16)(g_io_base + reg), v); }
static inline u32 io_readl(u16 reg) { return inl((u16)(g_io_base + reg)); }
static inline void io_writeb(u16 reg, u8 v) { outb((u16)(g_io_base + reg), v); }

static void udelay(usize n) {
    for (volatile usize i = 0; i < n; i++) {
        __asm__ volatile ("" : : : "memory");
    }
}

static u32 phys_addr(const void *p) {
    /* Current boot paging is identity-map; use virtual as physical. */
    return (u32)(usize)p;
}

static void uhci_reset(void) {
    io_writew(UHCI_USBCMD, (u16)(io_readw(UHCI_USBCMD) | USBCMD_HCRESET));
    for (usize i = 0; i < 200000; i++) {
        if ((io_readw(UHCI_USBCMD) & USBCMD_HCRESET) == 0) {
            break;
        }
    }
}

static void uhci_init_schedule(void) {
    for (usize i = 0; i < 1024; i++) {
        g_frame_list[i] = UHCI_LINK_TERMINATE;
    }

    g_async_qh.head_link_ptr = UHCI_LINK_TERMINATE;
    g_async_qh.element_link_ptr = UHCI_LINK_TERMINATE;

    /* Point every frame at the async QH. */
    const u32 qh_ptr = (phys_addr(&g_async_qh) & ~0xFU) | UHCI_LINK_QH;
    for (usize i = 0; i < 1024; i++) {
        g_frame_list[i] = qh_ptr;
    }

    io_writel(UHCI_FRBASEADD, phys_addr(g_frame_list));
    io_writew(UHCI_FRNUM, 0);
    io_writeb(UHCI_SOFMOD, 64);
    io_writew(UHCI_USBINTR, 0);
    io_writew(UHCI_USBSTS, 0xFFFF);
}

static void uhci_run(void) {
    u16 cmd = io_readw(UHCI_USBCMD);
    cmd |= USBCMD_RUN;
    cmd |= USBCMD_MAXP;
    io_writew(UHCI_USBCMD, cmd);
}

static u16 uhci_port_reg(u8 port) {
    return (port == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
}

static bool uhci_port_has_device(u8 port) {
    const u16 ps = io_readw(uhci_port_reg(port));
    return (ps & PORTSC_CCS) != 0;
}

static bool uhci_port_reset_enable(u8 port) {
    const u16 reg = uhci_port_reg(port);

    u16 ps = io_readw(reg);
    io_writew(reg, (u16)(ps | PORTSC_PR));
    udelay(200000);

    ps = io_readw(reg);
    io_writew(reg, (u16)(ps & (u16)~PORTSC_PR));
    udelay(200000);

    ps = io_readw(reg);
    /* Clear change bits by writing 1 to them (UHCI semantics). */
    io_writew(reg, (u16)(ps | PORTSC_CSC | PORTSC_PEC));
    udelay(50000);

    ps = io_readw(reg);
    /* Enable port. */
    io_writew(reg, (u16)(ps | PORTSC_PED));
    udelay(50000);

    ps = io_readw(reg);
    return (ps & PORTSC_PED) != 0;
}

static bool uhci_port_is_low_speed(u8 port) {
    const u16 ps = io_readw(uhci_port_reg(port));
    return (ps & PORTSC_LSDA) != 0;
}

static void td_init(uhci_td_t *td, u32 link, u32 ctrl, u32 token, u32 buf) {
    td->link_ptr = link;
    td->ctrl_status = ctrl;
    td->token = token;
    td->buffer = buf;
}

static u32 td_token(u8 pid, u8 dev, u8 endp, u16 maxlen, u8 data_toggle) {
    /* maxlen encoded as (len-1), 0x7FF for zero-length. */
    u16 len = maxlen;
    u32 len_field = (len == 0) ? 0x7FFU : (u32)((len - 1U) & 0x7FFU);
    return ((u32)pid) |
           ((u32)dev << 8) |
           ((u32)endp << 15) |
           ((u32)data_toggle << 19) |
           (len_field << 21);
}

static bool uhci_wait_td_done(uhci_td_t *td) {
    for (usize i = 0; i < 400000; i++) {
        if ((td->ctrl_status & TD_CTRL_ACTIVE) == 0) {
            return true;
        }
    }
    return false;
}

static bool uhci_control_transfer(u8 dev_addr, bool low_speed,
                                  const void *setup8,
                                  void *data, u16 data_len,
                                  bool data_in) {
    if (!setup8) {
        return false;
    }
    if (data_len > (u16)(sizeof(g_ctrl_buf) - 8)) {
        return false;
    }
    if (g_ep0_maxpkt == 0) {
        g_ep0_maxpkt = 8;
    }

    kmemcpy(g_ctrl_buf, setup8, 8);

    const u32 td_ls = low_speed ? TD_CTRL_LS : 0;
    const u32 td_common = TD_CTRL_ACTIVE | TD_CTRL_ERRCNT_3 | td_ls | TD_CTRL_SPD;

    const u16 pkt = g_ep0_maxpkt;
    u16 remaining = data_len;
    u8 toggle = 1;

    if (data && data_len && !data_in) {
        kmemcpy(g_ctrl_buf + 8, data, data_len);
    }

    /* SETUP stage (toggle 0). */
    const u32 first_data_link = (data_len == 0)
        ? ((phys_addr(&g_td_status) & ~0xFU) | UHCI_LINK_TD)
        : ((phys_addr(&g_td_data[0]) & ~0xFU) | UHCI_LINK_TD);

    td_init(&g_td_setup,
            first_data_link,
            td_common,
            td_token(USB_PID_SETUP, dev_addr, 0, 8, 0),
            phys_addr(g_ctrl_buf));

    /* DATA stage: one packet per TD (UHCI does not auto-split). */
    u8 td_count = 0;
    if (data_len != 0) {
        while (remaining != 0) {
            if (td_count >= (u8)(sizeof(g_td_data) / sizeof(g_td_data[0]))) {
                return false;
            }
            const u16 chunk = (remaining > pkt) ? pkt : remaining;
            remaining = (u16)(remaining - chunk);

            const bool last = (remaining == 0);
            const u32 next_link = last
                ? ((phys_addr(&g_td_status) & ~0xFU) | UHCI_LINK_TD)
                : ((phys_addr(&g_td_data[td_count + 1]) & ~0xFU) | UHCI_LINK_TD);

            td_init(&g_td_data[td_count],
                    next_link,
                    td_common,
                    td_token(data_in ? USB_PID_IN : USB_PID_OUT, dev_addr, 0, chunk, toggle),
                    phys_addr(g_ctrl_buf + 8 + (td_count * pkt)));

            toggle ^= 1;
            td_count++;
        }
    }

    /* STATUS stage: opposite direction, toggle 1, zero-length. */
    td_init(&g_td_status,
            UHCI_LINK_TERMINATE,
            td_common,
            td_token(data_in ? USB_PID_OUT : USB_PID_IN, dev_addr, 0, 0, 1),
            0);

    g_async_qh.element_link_ptr = (phys_addr(&g_td_setup) & ~0xFU) | UHCI_LINK_TD;

    if (!uhci_wait_td_done(&g_td_status)) {
        g_async_qh.element_link_ptr = UHCI_LINK_TERMINATE;
        return false;
    }

    g_async_qh.element_link_ptr = UHCI_LINK_TERMINATE;

    if (data && data_len && data_in) {
        kmemcpy(data, g_ctrl_buf + 8, data_len);
    }

    /* Check status bits for errors (stall/timeout/buffer/babble/bitstuff). */
    const u32 err_mask = (1U << 17) | (1U << 18) | (1U << 19) | (1U << 20) | (1U << 21) | (1U << 22);
    if ((g_td_setup.ctrl_status & err_mask) || (g_td_status.ctrl_status & err_mask)) {
        return false;
    }
    for (u8 i = 0; i < td_count; i++) {
        if (g_td_data[i].ctrl_status & err_mask) {
            return false;
        }
    }

    return true;
}

typedef struct __attribute__((packed)) {
    u8 bmRequestType;
    u8 bRequest;
    u16 wValue;
    u16 wIndex;
    u16 wLength;
} usb_setup_t;

static bool usb_get_device_desc8(u8 addr, bool low_speed, u8 *out8) {
    usb_setup_t s;
    s.bmRequestType = 0x80;
    s.bRequest = 6;
    s.wValue = 0x0100;
    s.wIndex = 0;
    s.wLength = 8;
    return uhci_control_transfer(addr, low_speed, &s, out8, 8, true);
}

static bool usb_set_address(u8 addr0, bool low_speed, u8 new_addr) {
    usb_setup_t s;
    s.bmRequestType = 0x00;
    s.bRequest = 5;
    s.wValue = new_addr;
    s.wIndex = 0;
    s.wLength = 0;
    return uhci_control_transfer(addr0, low_speed, &s, 0, 0, false);
}

static bool usb_get_config_desc(u8 addr, bool low_speed, u8 *buf, u16 len) {
    usb_setup_t s;
    s.bmRequestType = 0x80;
    s.bRequest = 6;
    s.wValue = 0x0200;
    s.wIndex = 0;
    s.wLength = len;
    return uhci_control_transfer(addr, low_speed, &s, buf, len, true);
}

static bool usb_set_configuration(u8 addr, bool low_speed, u8 cfg) {
    usb_setup_t s;
    s.bmRequestType = 0x00;
    s.bRequest = 9;
    s.wValue = cfg;
    s.wIndex = 0;
    s.wLength = 0;
    return uhci_control_transfer(addr, low_speed, &s, 0, 0, false);
}

static bool hid_set_protocol_boot(u8 addr, bool low_speed, u8 iface) {
    usb_setup_t s;
    s.bmRequestType = 0x21;
    s.bRequest = 0x0B; /* SET_PROTOCOL */
    s.wValue = 0;      /* boot protocol */
    s.wIndex = iface;
    s.wLength = 0;
    return uhci_control_transfer(addr, low_speed, &s, 0, 0, false);
}

static bool hid_set_idle(u8 addr, bool low_speed, u8 iface) {
    usb_setup_t s;
    s.bmRequestType = 0x21;
    s.bRequest = 0x0A; /* SET_IDLE */
    s.wValue = 0;
    s.wIndex = iface;
    s.wLength = 0;
    return uhci_control_transfer(addr, low_speed, &s, 0, 0, false);
}

static bool parse_hid_mouse_endpoint(const u8 *cfg, u16 cfg_len,
                                     u8 *out_cfg_value, u8 *out_iface, u8 *out_ep, u16 *out_maxpkt) {
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

        if (type == 4 && len >= 9) { /* INTERFACE */
            cur_iface = cfg[i + 2];
            iface_class = cfg[i + 5];
            iface_sub = cfg[i + 6];
            iface_proto = cfg[i + 7];
        } else if (type == 5 && len >= 7) { /* ENDPOINT */
            if (cur_iface != 0xFF &&
                iface_class == 0x03 && /* HID */
                iface_sub == 0x01 &&   /* Boot */
                iface_proto == 0x02) { /* Mouse */
                const u8 ep_addr = cfg[i + 2];
                const u8 attrs = cfg[i + 3];
                const u16 mps = (u16)cfg[i + 4] | ((u16)cfg[i + 5] << 8);
                const bool is_in = (ep_addr & 0x80) != 0;
                const u8 xfer_type = attrs & 0x03;
                if (is_in && xfer_type == 3 && mps > 0) { /* interrupt IN */
                    *out_cfg_value = cfg_value;
                    *out_iface = cur_iface;
                    *out_ep = (u8)(ep_addr & 0x0F);
                    *out_maxpkt = mps;
                    return true;
                }
            }
        }

        i = (u16)(i + len);
    }

    return false;
}

static bool usb_enumerate_mouse_on_port(u8 port) {
    if (!uhci_port_has_device(port)) {
        return false;
    }
    if (!uhci_port_reset_enable(port)) {
        return false;
    }

    g_dev_low_speed = uhci_port_is_low_speed(port);
    g_dev_addr = 0;
    g_mouse_ready = false;

    /* Device descriptor (first 8 bytes) at address 0. */
    u8 dd8[8];
    if (!usb_get_device_desc8(0, g_dev_low_speed, dd8)) {
        return false;
    }

    const u8 maxpkt0 = dd8[7];
    g_ep0_maxpkt = maxpkt0 ? maxpkt0 : 8;

    /* Assign address 1. */
    if (!usb_set_address(0, g_dev_low_speed, 1)) {
        return false;
    }
    udelay(200000);
    g_dev_addr = 1;

    /* Read config header to get total length. */
    u8 cfg_hdr[9];
    if (!usb_get_config_desc(g_dev_addr, g_dev_low_speed, cfg_hdr, 9)) {
        return false;
    }
    const u16 total_len = (u16)cfg_hdr[2] | ((u16)cfg_hdr[3] << 8);
    if (total_len < 9 || total_len > sizeof(g_ctrl_buf)) {
        return false;
    }

    u8 cfg_all[256];
    if (!usb_get_config_desc(g_dev_addr, g_dev_low_speed, cfg_all, total_len)) {
        return false;
    }

    u8 cfg_value = 0;
    u8 iface = 0;
    u8 ep = 0;
    u16 mps = 0;
    if (!parse_hid_mouse_endpoint(cfg_all, total_len, &cfg_value, &iface, &ep, &mps)) {
        return false;
    }

    if (!usb_set_configuration(g_dev_addr, g_dev_low_speed, cfg_value)) {
        return false;
    }
    (void)hid_set_protocol_boot(g_dev_addr, g_dev_low_speed, iface);
    (void)hid_set_idle(g_dev_addr, g_dev_low_speed, iface);

    g_mouse_ep = ep;
    g_mouse_maxpkt = mps;
    g_mouse_iface = iface;
    g_int_toggle = 0;

    /* Interrupt QH/TD setup (one IN TD, we re-arm it after completion). */
    g_int_qh.head_link_ptr = UHCI_LINK_TERMINATE;
    g_int_qh.element_link_ptr = UHCI_LINK_TERMINATE;

    const u32 td_ls = g_dev_low_speed ? TD_CTRL_LS : 0;
    td_init(&g_int_td,
            UHCI_LINK_TERMINATE,
            TD_CTRL_ACTIVE | TD_CTRL_ERRCNT_3 | td_ls | TD_CTRL_SPD,
            td_token(USB_PID_IN, g_dev_addr, g_mouse_ep, 3, g_int_toggle),
            phys_addr(g_int_buf));
    g_int_qh.element_link_ptr = (phys_addr(&g_int_td) & ~0xFU) | UHCI_LINK_TD;

    /* Put interrupt QH at start of async list. */
    g_int_qh.head_link_ptr = (phys_addr(&g_async_qh) & ~0xFU) | UHCI_LINK_QH;
    g_async_qh.head_link_ptr = UHCI_LINK_TERMINATE;

    /* Patch frame list to point to interrupt QH instead. */
    const u32 int_qh_ptr = (phys_addr(&g_int_qh) & ~0xFU) | UHCI_LINK_QH;
    for (usize i = 0; i < 1024; i++) {
        g_frame_list[i] = int_qh_ptr;
    }

    kmemset(g_last_report, 0, sizeof(g_last_report));
    g_mouse_ready = true;
    return true;
}

bool uhci_usb_ready(void) {
    return g_usb_ready && g_mouse_ready;
}

void uhci_usb_init(void) {
    g_usb_ready = false;
    g_io_base = 0;
    g_dev_addr = 0;
    g_ep0_maxpkt = 8;
    g_mouse_ready = false;
    g_int_toggle = 0;

    pci_init();

    /* UHCI is class 0x0C, subclass 0x03, progIF 0x00. */
    pci_addr_t uhci;
    if (!pci_find_class(0x0C, 0x03, 0x00, &uhci)) {
    log_write("usb(uhci): no controller found");
        return;
    }

    const u32 bar4 = pci_read_bar(uhci, 4);
    if ((bar4 & 0x01) == 0) {
        log_write("usb(uhci): BAR is not I/O");
        return;
    }

    g_io_base = (u16)(bar4 & 0xFFF0U);
    if (g_io_base == 0) {
        log_write("usb(uhci): I/O base is 0");
        return;
    }

    uhci_reset();
    uhci_init_schedule();
    uhci_run();

    /* Try enumerate on port 1 then port 2. */
    if (!usb_enumerate_mouse_on_port(0) && !usb_enumerate_mouse_on_port(1)) {
        log_write("usb(uhci): no HID mouse found");
        g_usb_ready = true;
        return;
    }

    log_write("usb(uhci): HID mouse ready");
    g_usb_ready = true;
}

static void usb_handle_mouse_report(const u8 *rep, usize len) {
    if (len < 3) {
        return;
    }

    const u8 buttons = rep[0];
    const i8 dx = (i8)rep[1];
    const i8 dy = (i8)rep[2];

    if (dx != 0 || dy != 0) {
        input_event_t move_evt;
        move_evt.type = INPUT_EVENT_MOUSE_MOVE;
        move_evt.a = (i32)dx;
        move_evt.b = (i32)(-dy);
        move_evt.c = 0;
        input_event_push(move_evt);
    }

    if (buttons != g_last_report[0]) {
        input_event_t btn_evt;
        btn_evt.type = INPUT_EVENT_MOUSE_BUTTON;
        btn_evt.a = (buttons & 0x01) ? 1 : 0; /* left */
        btn_evt.b = (buttons & 0x02) ? 1 : 0; /* right */
        btn_evt.c = (buttons & 0x04) ? 1 : 0; /* middle */
        input_event_push(btn_evt);
    }
}

void uhci_usb_poll(void) {
    if (!g_usb_ready || !g_mouse_ready) {
        return;
    }

    /* If TD completed, read report and re-arm. */
    if ((g_int_td.ctrl_status & TD_CTRL_ACTIVE) == 0) {
        const u32 err_mask = (1U << 17) | (1U << 18) | (1U << 19) | (1U << 20) | (1U << 21) | (1U << 22);
        const bool ok = (g_int_td.ctrl_status & err_mask) == 0;
        if (ok) {
            g_int_toggle ^= 1;
        }
        usb_handle_mouse_report(g_int_buf, 3);
        kmemcpy(g_last_report, g_int_buf, 3);

        /* Re-arm TD. */
        const u32 td_ls = g_dev_low_speed ? TD_CTRL_LS : 0;
        g_int_td.token = td_token(USB_PID_IN, g_dev_addr, g_mouse_ep, 3, g_int_toggle);
        g_int_td.ctrl_status = TD_CTRL_ACTIVE | TD_CTRL_ERRCNT_3 | td_ls | TD_CTRL_SPD;
    }
}
