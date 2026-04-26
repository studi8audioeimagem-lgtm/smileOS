#ifndef ATA_H
#define ATA_H

#include "types.h"

bool ata_pio_init(void);
bool ata_pio_read28(u32 lba, u8 sector_count, void *out512n);

#endif

