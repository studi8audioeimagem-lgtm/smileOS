#include "kernel.h"

/* Backends */
void uhci_usb_init(void);
void uhci_usb_poll(void);
bool uhci_usb_ready(void);

void xhci_usb_init(void);
void xhci_usb_poll(void);
bool xhci_usb_ready(void);

typedef enum {
    USB_BACKEND_NONE = 0,
    USB_BACKEND_XHCI,
    USB_BACKEND_UHCI
} usb_backend_t;

static usb_backend_t g_backend;

void usb_init(void) {
    g_backend = USB_BACKEND_NONE;

    xhci_usb_init();
    if (xhci_usb_ready()) {
        g_backend = USB_BACKEND_XHCI;
        return;
    }

    uhci_usb_init();
    if (uhci_usb_ready()) {
        g_backend = USB_BACKEND_UHCI;
        return;
    }
}

void usb_poll(void) {
    if (g_backend == USB_BACKEND_XHCI) {
        xhci_usb_poll();
    } else if (g_backend == USB_BACKEND_UHCI) {
        uhci_usb_poll();
    }
}

