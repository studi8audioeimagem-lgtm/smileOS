#include "pci.h"
#include "port.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static u32 pci_cfg_addr(u8 bus, u8 slot, u8 func, u8 offset) {
    return (u32)(0x80000000U |
                 ((u32)bus << 16) |
                 ((u32)slot << 11) |
                 ((u32)func << 8) |
                 ((u32)offset & 0xFCU));
}

static u32 pci_read32(u8 bus, u8 slot, u8 func, u8 offset) {
    outl(PCI_CONFIG_ADDRESS, pci_cfg_addr(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

static void pci_write32(u8 bus, u8 slot, u8 func, u8 offset, u32 value) {
    outl(PCI_CONFIG_ADDRESS, pci_cfg_addr(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

static u16 pci_read16(u8 bus, u8 slot, u8 func, u8 offset) {
    const u32 v = pci_read32(bus, slot, func, offset);
    const u32 shift = (u32)(offset & 2U) * 8U;
    return (u16)((v >> shift) & 0xFFFFU);
}

static void pci_write16(u8 bus, u8 slot, u8 func, u8 offset, u16 value) {
    const u32 v = pci_read32(bus, slot, func, offset);
    const u32 shift = (u32)(offset & 2U) * 8U;
    const u32 mask = 0xFFFFU << shift;
    const u32 nv = (v & ~mask) | ((u32)value << shift);
    pci_write32(bus, slot, func, offset, nv);
}

static u8 pci_read8(u8 bus, u8 slot, u8 func, u8 offset) {
    const u32 v = pci_read32(bus, slot, func, offset);
    const u32 shift = (u32)(offset & 3U) * 8U;
    return (u8)((v >> shift) & 0xFFU);
}

void pci_init(void) {
    /* Nothing to do for legacy I/O config space access. */
}

u16 pci_read_vendor_id(pci_addr_t addr) {
    return pci_read16(addr.bus, addr.slot, addr.func, 0x00);
}

u16 pci_read_device_id(pci_addr_t addr) {
    return pci_read16(addr.bus, addr.slot, addr.func, 0x02);
}

u8 pci_read_prog_if(pci_addr_t addr) {
    return pci_read8(addr.bus, addr.slot, addr.func, 0x09);
}

u8 pci_read_subclass(pci_addr_t addr) {
    return pci_read8(addr.bus, addr.slot, addr.func, 0x0A);
}

u8 pci_read_class(pci_addr_t addr) {
    return pci_read8(addr.bus, addr.slot, addr.func, 0x0B);
}

u32 pci_read_bar(pci_addr_t addr, u8 bar_index) {
    if (bar_index >= 6) {
        return 0;
    }
    return pci_read32(addr.bus, addr.slot, addr.func, (u8)(0x10 + (bar_index * 4)));
}

u16 pci_read_command(pci_addr_t addr) {
    return pci_read16(addr.bus, addr.slot, addr.func, 0x04);
}

void pci_write_command(pci_addr_t addr, u16 value) {
    pci_write16(addr.bus, addr.slot, addr.func, 0x04, value);
}

bool pci_find_class(u8 class_code, u8 subclass, u8 prog_if, pci_addr_t *out) {
    if (!out) {
        return false;
    }

    for (u16 bus = 0; bus < 256; bus++) {
        for (u8 slot = 0; slot < 32; slot++) {
            for (u8 func = 0; func < 8; func++) {
                pci_addr_t addr;
                addr.bus = (u8)bus;
                addr.slot = slot;
                addr.func = func;

                const u16 vendor = pci_read_vendor_id(addr);
                if (vendor == 0xFFFF) {
                    if (func == 0) {
                        break;
                    }
                    continue;
                }

                if (pci_read_class(addr) == class_code &&
                    pci_read_subclass(addr) == subclass &&
                    pci_read_prog_if(addr) == prog_if) {
                    *out = addr;
                    return true;
                }
            }
        }
    }

    return false;
}

bool pci_find_device(u16 vendor_id, u16 device_id, pci_addr_t *out) {
    if (!out) {
        return false;
    }

    for (u16 bus = 0; bus < 256; bus++) {
        for (u8 slot = 0; slot < 32; slot++) {
            for (u8 func = 0; func < 8; func++) {
                pci_addr_t addr;
                addr.bus = (u8)bus;
                addr.slot = slot;
                addr.func = func;

                const u16 vendor = pci_read_vendor_id(addr);
                if (vendor == 0xFFFF) {
                    if (func == 0) {
                        break;
                    }
                    continue;
                }
                if (vendor == vendor_id && pci_read_device_id(addr) == device_id) {
                    *out = addr;
                    return true;
                }
            }
        }
    }

    return false;
}
