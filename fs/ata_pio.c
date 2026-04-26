#include "kernel.h"
#include "port.h"
#include "ata.h"

/* Legacy ATA PIO (primary bus, master drive). Works well in QEMU with IDE. */

#define ATA_IO_BASE   0x1F0
#define ATA_CTRL_BASE 0x3F6

#define ATA_REG_DATA      0x00
#define ATA_REG_ERROR     0x01
#define ATA_REG_SECCOUNT0 0x02
#define ATA_REG_LBA0      0x03
#define ATA_REG_LBA1      0x04
#define ATA_REG_LBA2      0x05
#define ATA_REG_HDDEVSEL  0x06
#define ATA_REG_COMMAND   0x07
#define ATA_REG_STATUS    0x07

#define ATA_CMD_IDENTIFY  0xEC
#define ATA_CMD_READ_PIO  0x20

static bool g_ata_present;

static u8 ata_status(void) {
    return inb(ATA_IO_BASE + ATA_REG_STATUS);
}

static void ata_io_wait(void) {
    /* 400ns delay */
    inb(ATA_CTRL_BASE);
    inb(ATA_CTRL_BASE);
    inb(ATA_CTRL_BASE);
    inb(ATA_CTRL_BASE);
}

static bool ata_wait_bsy_clear(void) {
    for (usize i = 0; i < 200000; i++) {
        if ((ata_status() & 0x80) == 0) {
            return true;
        }
    }
    return false;
}

static bool ata_wait_drq_set(void) {
    for (usize i = 0; i < 200000; i++) {
        const u8 st = ata_status();
        if (st & 0x01) { /* ERR */
            return false;
        }
        if ((st & 0x08) != 0) {
            return true;
        }
    }
    return false;
}

static void ata_select_master(void) {
    outb(ATA_IO_BASE + ATA_REG_HDDEVSEL, 0xE0); /* LBA + master */
    ata_io_wait();
}

bool ata_pio_init(void) {
    g_ata_present = false;
    ata_select_master();

    /* IDENTIFY */
    if (!ata_wait_bsy_clear()) {
        return false;
    }

    outb(ATA_IO_BASE + ATA_REG_SECCOUNT0, 0);
    outb(ATA_IO_BASE + ATA_REG_LBA0, 0);
    outb(ATA_IO_BASE + ATA_REG_LBA1, 0);
    outb(ATA_IO_BASE + ATA_REG_LBA2, 0);
    outb(ATA_IO_BASE + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_io_wait();

    const u8 st = ata_status();
    if (st == 0) {
        return false; /* no device */
    }

    if (!ata_wait_bsy_clear()) {
        return false;
    }
    if (!ata_wait_drq_set()) {
        return false;
    }

    /* Read 256 words of IDENTIFY data and discard. */
    for (usize i = 0; i < 256; i++) {
        (void)inw(ATA_IO_BASE + ATA_REG_DATA);
    }

    g_ata_present = true;
    log_write("fs: ata pio present");
    return true;
}

bool ata_pio_read28(u32 lba, u8 sector_count, void *out512n) {
    if (!g_ata_present || !out512n || sector_count == 0) {
        return false;
    }
    if (lba & 0xF0000000U) {
        return false; /* LBA28 only */
    }

    u16 *out = (u16 *)out512n;

    for (u32 s = 0; s < sector_count; s++) {
        const u32 l = lba + s;
        ata_select_master();
        if (!ata_wait_bsy_clear()) {
            return false;
        }

        outb(ATA_IO_BASE + ATA_REG_SECCOUNT0, 1);
        outb(ATA_IO_BASE + ATA_REG_LBA0, (u8)(l & 0xFF));
        outb(ATA_IO_BASE + ATA_REG_LBA1, (u8)((l >> 8) & 0xFF));
        outb(ATA_IO_BASE + ATA_REG_LBA2, (u8)((l >> 16) & 0xFF));
        outb(ATA_IO_BASE + ATA_REG_HDDEVSEL, (u8)(0xE0 | ((l >> 24) & 0x0F)));
        outb(ATA_IO_BASE + ATA_REG_COMMAND, ATA_CMD_READ_PIO);
        ata_io_wait();

        if (!ata_wait_bsy_clear()) {
            return false;
        }
        if (!ata_wait_drq_set()) {
            return false;
        }

        /* 256 words => 512 bytes */
        for (usize i = 0; i < 256; i++) {
            out[s * 256 + i] = inw(ATA_IO_BASE + ATA_REG_DATA);
        }
        ata_io_wait();
    }

    return true;
}

