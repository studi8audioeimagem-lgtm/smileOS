#include "kernel.h"
#include "string.h"
#include "ata.h"
#include "fat32.h"

typedef struct {
    bool mounted;
    u32 part_lba;
    u16 bytes_per_sector;
    u8 sectors_per_cluster;
    u16 reserved_sectors;
    u8 num_fats;
    u32 fat_size_sectors;
    u32 fat_lba;
    u32 data_lba;
    u32 root_cluster;
} fat32_fs_t;

static fat32_fs_t g_fs;

static u16 rd16(const u8 *p) { return (u16)p[0] | ((u16)p[1] << 8); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static bool read_sector(u32 lba, u8 *out512) {
    return ata_pio_read28(lba, 1, out512);
}

static u32 cluster_to_lba(u32 clus) {
    return g_fs.data_lba + (clus - 2U) * (u32)g_fs.sectors_per_cluster;
}

static u32 fat_get(u32 clus) {
    /* FAT32 entry = 4 bytes, low 28 bits used. */
    const u32 fat_off = clus * 4U;
    const u32 sec = fat_off / g_fs.bytes_per_sector;
    const u32 off = fat_off % g_fs.bytes_per_sector;

    u8 buf[512];
    if (!read_sector(g_fs.fat_lba + sec, buf)) {
        return 0x0FFFFFFFU;
    }

    /* Assumes 512B sectors. */
    if (off + 4 > 512) {
        return 0x0FFFFFFFU;
    }

    return rd32(&buf[off]) & 0x0FFFFFFFU;
}

static bool is_eoc(u32 clus) {
    return clus >= 0x0FFFFFF8U;
}

static void trim_spaces(char *s) {
    if (!s) return;
    for (i32 i = 30; i >= 0; i--) {
        if (s[i] == ' ' || s[i] == 0) {
            s[i] = 0;
            continue;
        }
        break;
    }
}

static void make_83_name(char *out, usize cap, const u8 *ent) {
    if (!out || cap == 0 || !ent) return;
    kmemset(out, 0, cap);

    char base[9];
    char ext[4];
    kmemset(base, 0, sizeof(base));
    kmemset(ext, 0, sizeof(ext));

    for (usize i = 0; i < 8; i++) base[i] = (char)ent[i];
    for (usize i = 0; i < 3; i++) ext[i] = (char)ent[8 + i];
    base[8] = 0;
    ext[3] = 0;
    trim_spaces(base);
    trim_spaces(ext);

    /* Uppercase already; keep as-is. */
    if (ext[0] == 0) {
        kmemcpy(out, base, (kstrlen(base) + 1 < cap) ? kstrlen(base) + 1 : cap - 1);
        return;
    }

    usize bi = 0;
    for (; base[bi] != 0 && bi + 1 < cap; bi++) out[bi] = base[bi];
    if (bi + 1 < cap) out[bi++] = '.';
    for (usize ei = 0; ext[ei] != 0 && bi + 1 < cap; ei++) out[bi++] = ext[ei];
    out[bi] = 0;
}

bool fat32_mount_primary(void) {
    kmemset(&g_fs, 0, sizeof(g_fs));

    if (!ata_pio_init()) {
        log_write("fat32: no ata device");
        return false;
    }

    u8 mbr[512];
    if (!read_sector(0, mbr)) {
        log_write("fat32: cannot read mbr");
        return false;
    }

    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        log_write("fat32: bad mbr sig");
        return false;
    }

    u32 part_lba = 0;
    for (u32 i = 0; i < 4; i++) {
        const u8 *pe = &mbr[446 + i * 16];
        const u8 type = pe[4];
        /* FAT32: 0x0B/0x0C (and hidden variants 0x1B/0x1C). */
        if (type == 0x0B || type == 0x0C || type == 0x1B || type == 0x1C) {
            part_lba = rd32(&pe[8]);
            break;
        }
    }

    u8 bpb[512];
    if (!part_lba) {
        /* Superfloppy fallback: filesystem starts at LBA0 (common for raw images). */
        if (!read_sector(0, bpb)) {
            log_write("fat32: cannot read bpb");
            return false;
        }
        part_lba = 0;
    } else {
        if (!read_sector(part_lba, bpb)) {
            log_write("fat32: cannot read bpb");
            return false;
        }
    }

    const u16 bps = rd16(&bpb[11]);
    const u8 spc = bpb[13];
    const u16 rsv = rd16(&bpb[14]);
    const u8 nf = bpb[16];
    const u32 fatsz = rd32(&bpb[36]);
    const u32 rootc = rd32(&bpb[44]);

    /* If this isn't actually a FAT32 BPB, bail. */
    bool looks_fat32 = true;
    if (bpb[82] != 'F' || bpb[83] != 'A' || bpb[84] != 'T' || bpb[85] != '3' || bpb[86] != '2') {
        /* Some tools don't fill BS_FilSysType reliably; accept if core fields look sane. */
        looks_fat32 = false;
    }

    if (bps != 512 || spc == 0 || nf == 0 || fatsz == 0 || rootc < 2) {
        log_write("fat32: unsupported bpb");
        return false;
    }
    if (!looks_fat32 && part_lba == 0) {
        /* Reduce false positives when probing superfloppy images. */
        log_write("fat32: no partition and bpb not marked FAT32");
        return false;
    }

    g_fs.mounted = true;
    g_fs.part_lba = part_lba;
    g_fs.bytes_per_sector = bps;
    g_fs.sectors_per_cluster = spc;
    g_fs.reserved_sectors = rsv;
    g_fs.num_fats = nf;
    g_fs.fat_size_sectors = fatsz;
    g_fs.fat_lba = part_lba + (u32)rsv;
    g_fs.data_lba = g_fs.fat_lba + (u32)nf * fatsz;
    g_fs.root_cluster = rootc;

    log_write("fat32: mounted");
    return true;
}

usize fat32_list_root(fat32_dirent_t *out, usize cap) {
    if (!g_fs.mounted || !out || cap == 0) {
        return 0;
    }

    usize count = 0;
    u32 clus = g_fs.root_cluster;
    u8 sec[512];

    while (!is_eoc(clus) && clus >= 2 && count < cap) {
        const u32 lba0 = cluster_to_lba(clus);
        for (u32 si = 0; si < g_fs.sectors_per_cluster && count < cap; si++) {
            if (!read_sector(lba0 + si, sec)) {
                return count;
            }
            for (u32 off = 0; off < 512; off += 32) {
                const u8 *e = &sec[off];
                if (e[0] == 0x00) {
                    return count;
                }
                if (e[0] == 0xE5) {
                    continue;
                }
                const u8 attr = e[11];
                if (attr == 0x0F) { /* LFN */
                    continue;
                }
                if (attr & 0x08) { /* volume label */
                    continue;
                }

                fat32_dirent_t *de = &out[count];
                kmemset(de, 0, sizeof(*de));
                make_83_name(de->name, sizeof(de->name), e);
                const u16 hi = rd16(&e[20]);
                const u16 lo = rd16(&e[26]);
                de->first_cluster = ((u32)hi << 16) | (u32)lo;
                de->size = rd32(&e[28]);
                de->is_dir = (attr & 0x10) != 0;

                if (de->name[0] != 0) {
                    count++;
                    if (count >= cap) {
                        return count;
                    }
                }
            }
        }
        clus = fat_get(clus);
    }

    return count;
}

bool fat32_read_file(u32 first_cluster, u32 size, void *out, u32 out_cap, u32 *out_len) {
    if (!g_fs.mounted || !out || out_cap == 0) {
        return false;
    }
    if (out_len) *out_len = 0;
    if (first_cluster < 2) {
        return false;
    }

    u32 remaining = size;
    if (remaining > out_cap) remaining = out_cap;

    u8 *dst = (u8 *)out;
    u8 sec[512];
    u32 written = 0;

    u32 clus = first_cluster;
    while (!is_eoc(clus) && clus >= 2 && written < remaining) {
        const u32 lba0 = cluster_to_lba(clus);
        for (u32 si = 0; si < g_fs.sectors_per_cluster && written < remaining; si++) {
            if (!read_sector(lba0 + si, sec)) {
                if (out_len) *out_len = written;
                return false;
            }
            u32 to_copy = 512;
            if (to_copy > (remaining - written)) {
                to_copy = remaining - written;
            }
            for (u32 i = 0; i < to_copy; i++) {
                dst[written + i] = sec[i];
            }
            written += to_copy;
        }
        clus = fat_get(clus);
    }

    if (out_len) *out_len = written;
    return true;
}
