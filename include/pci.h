#ifndef PCI_H
#define PCI_H

#include "types.h"

typedef struct {
    u8 bus;
    u8 slot;
    u8 func;
} pci_addr_t;

void pci_init(void);
bool pci_find_class(u8 class_code, u8 subclass, u8 prog_if, pci_addr_t *out);
bool pci_find_device(u16 vendor_id, u16 device_id, pci_addr_t *out);
u16 pci_read_vendor_id(pci_addr_t addr);
u16 pci_read_device_id(pci_addr_t addr);
u8 pci_read_class(pci_addr_t addr);
u8 pci_read_subclass(pci_addr_t addr);
u8 pci_read_prog_if(pci_addr_t addr);
u32 pci_read_bar(pci_addr_t addr, u8 bar_index);
u16 pci_read_command(pci_addr_t addr);
void pci_write_command(pci_addr_t addr, u16 value);

#endif
