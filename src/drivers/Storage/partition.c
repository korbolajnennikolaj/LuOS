#include "drivers/Storage/partition.h"

#include "components/logger.h"
#include "components/Memory/heap.h"
#include "kernel/scheduler/spinlock.h"

#include <string.h>

typedef struct __attribute__((packed)) {
    uint8_t status;
    uint8_t chs_start[3];
    uint8_t type;
    uint8_t chs_end[3];
    uint32_t lba_start;
    uint32_t num_sectors;
} mbr_entry_t;

#define MBR_ENTRY_OFFSET 0x1BE
#define MBR_SIG_OFFSET 0x1FE

static int read_sector(struct block_device *disk, uint64_t lba, void *buf) {
    if (disk->read_sectors(disk, lba, 1, buf) != 0) {
        LOG_WARNING("%s: failed to read LBA %llu", disk->name, (unsigned long long)lba);
        return -1;
    }
    return 0;
}

static int read_bytes_at(struct block_device *disk, uint64_t byte_off, void *out, uint32_t len) {
    uint32_t ss = disk->sector_size ? disk->sector_size : 512;
    uint64_t lba = byte_off / ss;
    uint32_t off = (uint32_t)(byte_off % ss);
    uint32_t sectors_needed = (off + len + ss - 1) / ss;
    if (sectors_needed == 0) sectors_needed = 1;

    uint8_t *buf = kmalloc((size_t)sectors_needed * ss);
    if (!buf) return -1;
    if (disk->read_sectors(disk, lba, sectors_needed, buf) != 0) {
        LOG_WARNING("%s: failed to read %u sector(s) at LBA %llu", disk->name, sectors_needed, (unsigned long long)lba);
        kfree(buf);
        return -1;
    }
    memcpy(out, buf + off, len);
    kfree(buf);
    return 0;
}

static bool mbr_entry_plausible(const mbr_entry_t *e, uint64_t disk_sectors) {
    if (e->type == 0 && e->lba_start == 0 && e->num_sectors == 0) return true;
    if (e->lba_start == 0 || e->num_sectors == 0) return false;
    if ((uint64_t)e->lba_start + e->num_sectors > disk_sectors) return false;
    return true;
}

typedef struct __attribute__((packed)) {
    char signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t size_of_partition_entry;
    uint32_t partition_entry_array_crc32;
} gpt_header_t;

static int scan_gpt(struct block_device *disk, partition_info_t *out, int max, int *out_count) {
    uint32_t ss = disk->sector_size ? disk->sector_size : 512;
    uint8_t *buf = kmalloc(ss);
    if (!buf) return -1;
    if (read_sector(disk, 1, buf) != 0) { kfree(buf); return -1; }

    gpt_header_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    kfree(buf);

    if (memcmp(hdr.signature, "EFI PART", 8) != 0) return -1;

    uint32_t entry_size = hdr.size_of_partition_entry ? hdr.size_of_partition_entry : 128;
    uint32_t num_entries = hdr.num_partition_entries;
    if (num_entries > 512) {
        LOG_WARNING("%s: GPT claims %u entries, limiting to 512", disk->name, num_entries);
        num_entries = 512;
    }

    LOG_DEBUG("%s: GPT revision 0x%08x, %u entries x %u bytes at LBA %llu", disk->name,
              hdr.revision, num_entries, entry_size, (unsigned long long)hdr.partition_entry_lba);

    int count = 0;
    static const uint8_t zero_guid[16] = {0};

    for (uint32_t i = 0; i < num_entries && count < max; i++) {
        uint64_t entry_off = hdr.partition_entry_lba * ss + (uint64_t)i * entry_size;

        uint8_t type_guid[16];
        if (read_bytes_at(disk, entry_off, type_guid, 16) != 0) break;
        if (memcmp(type_guid, zero_guid, 16) == 0) continue;

        uint64_t starting_lba, ending_lba;
        read_bytes_at(disk, entry_off + 32, &starting_lba, 8);
        read_bytes_at(disk, entry_off + 40, &ending_lba, 8);
        if (ending_lba < starting_lba) {
            LOG_WARNING("%s: GPT entry %u has end LBA %llu before start %llu, skipping", disk->name, i,
                        (unsigned long long)ending_lba, (unsigned long long)starting_lba);
            continue;
        }

        partition_info_t *p = &out[count];
        memset(p, 0, sizeof(*p));
        p->start_lba = starting_lba;
        p->sector_count = ending_lba - starting_lba + 1;
        p->mbr_type = 0;
        memcpy(p->gpt_type_guid, type_guid, 16);
        p->bootable = false;
        p->is_gpt = true;
        count++;
    }

    *out_count = count;
    return 0;
}

static int scan_mbr(struct block_device *disk, partition_info_t *out, int max, int *out_count) {
    uint32_t ss = disk->sector_size ? disk->sector_size : 512;
    uint8_t *buf = kmalloc(ss);
    if (!buf) return -1;
    if (read_sector(disk, 0, buf) != 0) { kfree(buf); return -1; }

    if (buf[MBR_SIG_OFFSET] != 0x55 || buf[MBR_SIG_OFFSET + 1] != 0xAA) {
        LOG_DEBUG("%s: no MBR signature (0x%02x%02x)", disk->name,
                  (unsigned)buf[MBR_SIG_OFFSET + 1], (unsigned)buf[MBR_SIG_OFFSET]);
        kfree(buf);
        return -1;
    }

    mbr_entry_t entries[4];
    memcpy(entries, buf + MBR_ENTRY_OFFSET, sizeof(entries));
    kfree(buf);

    for (int i = 0; i < 4; i++) {
        if (entries[i].type == PART_TYPE_GPT_PROTECTIVE) {
            if (scan_gpt(disk, out, max, out_count) == 0) return 0;
            LOG_WARNING("%s: protective MBR found but GPT header is invalid", disk->name);
            break;
        }
    }

    bool any_nonempty = false;
    for (int i = 0; i < 4; i++) {
        if (!mbr_entry_plausible(&entries[i], disk->sector_count)) {
            LOG_DEBUG("%s: MBR entry %d implausible (type 0x%02x, start %u, %u sectors), not a partition table",
                      disk->name, i, (unsigned)entries[i].type, entries[i].lba_start, entries[i].num_sectors);
            return -1;
        }
        if (entries[i].type != 0) any_nonempty = true;
    }
    if (!any_nonempty) return -1;

    int count = 0;

    for (int i = 0; i < 4 && count < max; i++) {
        if (entries[i].type == 0) continue;

        if (entries[i].type == PART_TYPE_EXTENDED_CHS || entries[i].type == PART_TYPE_EXTENDED_LBA) {
            uint64_t extended_base = entries[i].lba_start;
            uint64_t next_ebr = extended_base;
            int guard = 0;

            while (next_ebr != 0 && count < max && guard++ < 128) {
                uint8_t *ebr = kmalloc(ss);
                if (!ebr) break;
                if (read_sector(disk, next_ebr, ebr) != 0) { kfree(ebr); break; }
                if (ebr[MBR_SIG_OFFSET] != 0x55 || ebr[MBR_SIG_OFFSET + 1] != 0xAA) { kfree(ebr); break; }

                mbr_entry_t ebr_entries[4];
                memcpy(ebr_entries, ebr + MBR_ENTRY_OFFSET, sizeof(ebr_entries));
                kfree(ebr);

                if (ebr_entries[0].type != 0 && ebr_entries[0].num_sectors != 0) {
                    partition_info_t *p = &out[count];
                    memset(p, 0, sizeof(*p));
                    p->start_lba = next_ebr + ebr_entries[0].lba_start;
                    p->sector_count = ebr_entries[0].num_sectors;
                    p->mbr_type = ebr_entries[0].type;
                    p->bootable = (ebr_entries[0].status & 0x80) != 0;
                    p->is_gpt = false;
                    count++;
                }

                if ((ebr_entries[1].type == PART_TYPE_EXTENDED_CHS ||
                     ebr_entries[1].type == PART_TYPE_EXTENDED_LBA) && ebr_entries[1].lba_start != 0) {
                    next_ebr = extended_base + ebr_entries[1].lba_start;
                } else {
                    next_ebr = 0;
                }
            }
            if (guard >= 128)
                LOG_WARNING("%s: extended partition chain too long, stopped after 128 EBRs", disk->name);
            continue;
        }

        partition_info_t *p = &out[count];
        memset(p, 0, sizeof(*p));
        p->start_lba = entries[i].lba_start;
        p->sector_count = entries[i].num_sectors;
        p->mbr_type = entries[i].type;
        p->bootable = (entries[i].status & 0x80) != 0;
        p->is_gpt = false;
        count++;
    }

    *out_count = count;
    return 0;
}

int partition_scan(struct block_device *disk, partition_info_t *out, int max, int *out_count) {
    if (!disk || !out || !out_count || max <= 0) return -1;
    *out_count = 0;

    int n = 0;
    if (scan_gpt(disk, out, max, &n) == 0 && n > 0) { *out_count = n; goto name_them; }
    if (scan_mbr(disk, out, max, &n) == 0) { *out_count = n; goto name_them; }

    return 0;

name_them:
    for (int i = 0; i < *out_count; i++) {
        char base[16];
        strncpy(base, disk->name, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';

        char num[8]; int nlen = 0;
        {
            int v = i + 1, tl = 0; char tmp[8];
            while (v > 0) { tmp[tl++] = (char)('0' + (v % 10)); v /= 10; }
            for (int k = tl - 1; k >= 0; k--) num[nlen++] = tmp[k];
            num[nlen] = '\0';
        }

        size_t bl = strlen(base);
        size_t total = bl + (size_t)nlen;
        if (total >= sizeof(out[i].name)) total = sizeof(out[i].name) - 1;
        memset(out[i].name, 0, sizeof(out[i].name));
        size_t copy_base = bl < total ? bl : total;
        memcpy(out[i].name, base, copy_base);
        size_t remain = total - copy_base;
        memcpy(out[i].name + copy_base, num, remain);
    }
    return 0;
}

typedef struct partition_device {
    struct block_device *parent;
    uint64_t start_lba;
    uint64_t sector_count;
} partition_device_t;

static partition_device_t g_partition_storage[MAX_BLOCK_DEVICES];
static struct block_device g_partition_blkdevs[MAX_BLOCK_DEVICES];
static int g_partition_count = 0;
static spinlock_t partition_lock = SPINLOCK_INIT;

static int part_read_sectors(struct block_device *self, uint64_t lba, uint32_t count, void *buf) {
    partition_device_t *p = (partition_device_t *)self->priv;
    if (lba + count > p->sector_count) return -1;
    return p->parent->read_sectors(p->parent, p->start_lba + lba, count, buf);
}

static int part_write_sectors(struct block_device *self, uint64_t lba, uint32_t count, void *buf) {
    partition_device_t *p = (partition_device_t *)self->priv;
    if (lba + count > p->sector_count) return -1;
    return p->parent->write_sectors(p->parent, p->start_lba + lba, count, buf);
}

static int part_flush(struct block_device *self) {
    partition_device_t *p = (partition_device_t *)self->priv;
    return p->parent->flush ? p->parent->flush(p->parent) : 0;
}

static int partition_find_free_slot(void) {
    for (int i = 0; i < MAX_BLOCK_DEVICES; i++)
        if (!g_partition_storage[i].parent) return i;
    return -1;
}

int partition_unregister_all(struct block_device *disk) {
    if (!disk) return 0;

    int removed = 0;
    spin_lock(&partition_lock);
    for (int i = 0; i < MAX_BLOCK_DEVICES; i++) {
        if (g_partition_storage[i].parent != disk) continue;

        block_device_unregister(&g_partition_blkdevs[i]);
        memset(&g_partition_blkdevs[i], 0, sizeof(g_partition_blkdevs[i]));
        g_partition_storage[i].parent = NULL;
        g_partition_storage[i].start_lba = 0;
        g_partition_storage[i].sector_count = 0;
        if (g_partition_count > 0) g_partition_count--;
        removed++;
    }
    spin_unlock(&partition_lock);
    return removed;
}

int partition_register_all(struct block_device *disk) {
    if (!disk) return 0;

    spin_lock(&partition_lock);

    for (int i = 0; i < MAX_BLOCK_DEVICES; i++) {
        if (g_partition_storage[i].parent == disk) {
            LOG_DEBUG("partitions for '%s' already registered", disk->name);
            spin_unlock(&partition_lock);
            return 0;
        }
    }

    partition_info_t infos[PART_MAX_PER_DISK];
    int n = 0;
    if (partition_scan(disk, infos, PART_MAX_PER_DISK, &n) != 0 || n == 0) {
        LOG_DEBUG("no partition table found on '%s', use the disk directly", disk->name);
        spin_unlock(&partition_lock);
        return 0;
    }

    int registered = 0;
    for (int i = 0; i < n; i++) {
        int slot = partition_find_free_slot();
        if (slot < 0) {
            LOG_WARNING("partition table full, %d partition(s) of '%s' not registered", n - i, disk->name);
            break;
        }

        partition_device_t *pd = &g_partition_storage[slot];
        struct block_device *bd = &g_partition_blkdevs[slot];

        pd->parent = disk;
        pd->start_lba = infos[i].start_lba;
        pd->sector_count = infos[i].sector_count;

        memset(bd, 0, sizeof(*bd));
        strncpy(bd->name, infos[i].name, sizeof(bd->name) - 1);
        bd->sector_count = infos[i].sector_count;
        bd->sector_size = disk->sector_size;
        bd->priv = pd;
        bd->read_sectors = part_read_sectors;
        bd->write_sectors = part_write_sectors;
        bd->flush = part_flush;

        block_device_register(bd);
        g_partition_count++;
        registered++;

        LOG_INFO("partition '%s': LBA %llu..%llu (%llu MB, %s type 0x%02x)", bd->name,
                 (unsigned long long)infos[i].start_lba,
                 (unsigned long long)(infos[i].start_lba + infos[i].sector_count - 1),
                 (unsigned long long)((infos[i].sector_count * (disk->sector_size ? disk->sector_size : 512)) >> 20),
                 infos[i].is_gpt ? "GPT" : "MBR", (unsigned)infos[i].mbr_type);
    }

    spin_unlock(&partition_lock);
    return registered;
}

bool partition_lookup(struct block_device *dev, struct block_device **parent, uint64_t *start_lba) {
    bool found = false;
    spin_lock(&partition_lock);
    for (int i = 0; i < MAX_BLOCK_DEVICES; i++) {
        if (!g_partition_storage[i].parent || &g_partition_blkdevs[i] != dev) continue;
        if (parent) *parent = g_partition_storage[i].parent;
        if (start_lba) *start_lba = g_partition_storage[i].start_lba;
        found = true;
        break;
    }
    spin_unlock(&partition_lock);
    return found;
}

int partition_set_mbr_type(struct block_device *part, uint8_t type) {
    struct block_device *disk = NULL;
    uint64_t start = 0;
    if (!partition_lookup(part, &disk, &start)) return -1;

    uint32_t ss = disk->sector_size ? disk->sector_size : 512;
    uint8_t *buf = kmalloc(ss);
    if (!buf) return -1;
    if (read_sector(disk, 0, buf) != 0) { kfree(buf); return -1; }

    int ret = 1;
    if (buf[MBR_SIG_OFFSET] == 0x55 && buf[MBR_SIG_OFFSET + 1] == 0xAA) {
        for (int i = 0; i < 4; i++) {
            mbr_entry_t e;
            memcpy(&e, buf + MBR_ENTRY_OFFSET + i * sizeof(mbr_entry_t), sizeof(e));
            if (e.type == PART_TYPE_GPT_PROTECTIVE) break;
            if (e.type == 0 || e.type == PART_TYPE_EXTENDED_CHS || e.type == PART_TYPE_EXTENDED_LBA) continue;
            if (e.lba_start != start) continue;
            if (e.type == type) { ret = 0; break; }
            buf[MBR_ENTRY_OFFSET + i * sizeof(mbr_entry_t) + 4] = type;
            ret = disk->write_sectors(disk, 0, 1, buf) == 0 ? 0 : -1;
            if (ret == 0) LOG_INFO("%s: MBR partition type set to 0x%02x", part->name, (unsigned)type);
            break;
        }
    }
    kfree(buf);
    return ret;
}
