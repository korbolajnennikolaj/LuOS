#include "fs/exfat.h"

#include "components/drivers.h"
#include "components/logger.h"
#include "components/Memory/heap.h"
#include "drivers/Timer/rtc_driver.h"
#include "drivers/Timer/timer.h"

#include <string.h>

#define EXFAT_CLUSTER_EOC 0xFFFFFFFFu
#define EXFAT_CLUSTER_EOC_MIN 0xFFFFFFF8u
#define EXFAT_CLUSTER_BAD 0xFFFFFFF7u
#define EXFAT_UPCASE_ENTRIES 65536u

typedef struct {
    uint16_t attributes;
    exfat_chain_t chain;
    uint64_t data_length;
    uint64_t valid_data_length;
    exfat_loc_t loc;
    char name_utf8[FS_MAX_NAME + 1];
} exfat_entry_info_t;

typedef struct {
    uint8_t *buf;
    uint32_t loaded;
    uint32_t cache_index;
    uint32_t cache_cluster;
} exfat_cursor_t;

static inline uint64_t cluster_to_lba(exfat_fs_t *fs, uint32_t cluster) {
    return (uint64_t)fs->cluster_heap_offset_lba + (uint64_t)(cluster - 2) * fs->sectors_per_cluster;
}

static inline bool cluster_valid(exfat_fs_t *fs, uint32_t cluster) {
    return cluster >= 2 && (cluster - 2) < fs->cluster_count;
}

static int read_cluster(exfat_fs_t *fs, uint32_t cluster, void *buf) {
    if (!cluster_valid(fs, cluster)) return FS_ERR_PARAM;
    if (fs->dev->read_sectors(fs->dev, cluster_to_lba(fs, cluster), fs->sectors_per_cluster, buf) != 0) {
        LOG_ERROR("%s: failed to read cluster %u", fs->dev->name, cluster);
        return FS_ERR_IO;
    }
    return FS_OK;
}

static int write_cluster(exfat_fs_t *fs, uint32_t cluster, const void *buf) {
    if (!cluster_valid(fs, cluster)) return FS_ERR_PARAM;
    if (fs->dev->write_sectors(fs->dev, cluster_to_lba(fs, cluster), fs->sectors_per_cluster, (void *)buf) != 0) {
        LOG_ERROR("%s: failed to write cluster %u", fs->dev->name, cluster);
        return FS_ERR_IO;
    }
    return FS_OK;
}

static int zero_cluster(exfat_fs_t *fs, uint32_t cluster) {
    uint8_t *buf = kmalloc(fs->cluster_size);
    if (!buf) return FS_ERR_NOMEM;
    memset(buf, 0, fs->cluster_size);
    int r = write_cluster(fs, cluster, buf);
    kfree(buf);
    return r;
}

static int fat_load_sector(exfat_fs_t *fs, uint32_t sector) {
    if (fs->fat_cache_sector == sector) return FS_OK;
    if (fs->dev->read_sectors(fs->dev, sector, 1, fs->fat_cache) != 0) {
        LOG_ERROR("%s: failed to read FAT sector %u", fs->dev->name, sector);
        fs->fat_cache_sector = UINT32_MAX;
        return FS_ERR_IO;
    }
    fs->fat_cache_sector = sector;
    return FS_OK;
}

static uint32_t get_fat_entry(exfat_fs_t *fs, uint32_t cluster) {
    uint32_t byte_off = cluster * 4;
    uint32_t sector = fs->fat_offset_lba + (byte_off / fs->bytes_per_sector);
    uint32_t off = byte_off % fs->bytes_per_sector;
    if (fat_load_sector(fs, sector) != FS_OK) return EXFAT_CLUSTER_EOC;
    uint32_t raw;
    memcpy(&raw, fs->fat_cache + off, 4);
    return raw;
}

static int set_fat_entry(exfat_fs_t *fs, uint32_t cluster, uint32_t value) {
    uint32_t byte_off = cluster * 4;
    uint32_t rel = byte_off / fs->bytes_per_sector;
    uint32_t off = byte_off % fs->bytes_per_sector;
    if (fat_load_sector(fs, fs->fat_offset_lba + rel) != FS_OK) return FS_ERR_IO;
    memcpy(fs->fat_cache + off, &value, 4);
    for (uint32_t f = 0; f < fs->number_of_fats; f++) {
        uint32_t sector = fs->fat_offset_lba + f * fs->fat_length_sectors + rel;
        if (fs->dev->write_sectors(fs->dev, sector, 1, fs->fat_cache) != 0) {
            LOG_ERROR("%s: failed to write FAT sector %u", fs->dev->name, sector);
            return FS_ERR_IO;
        }
    }
    return FS_OK;
}

static uint32_t cluster_for_index(exfat_fs_t *fs, uint32_t first_cluster, bool no_fat_chain,
                                   uint32_t *cache_index, uint32_t *cache_cluster, uint32_t index) {
    if (first_cluster < 2) return 0;

    if (no_fat_chain) {
        uint32_t c = first_cluster + index;
        if (!cluster_valid(fs, c)) return 0;
        return c;
    }

    uint32_t start_i = 0;
    uint32_t cur = first_cluster;
    if (cache_index && cache_cluster && index >= *cache_index && *cache_cluster >= 2) {
        start_i = *cache_index;
        cur = *cache_cluster;
    }

    for (uint32_t i = start_i; i < index; i++) {
        uint32_t next = get_fat_entry(fs, cur);
        if (next < 2 || next >= EXFAT_CLUSTER_EOC_MIN) return 0;
        cur = next;
    }

    if (cache_index && cache_cluster) { *cache_index = index; *cache_cluster = cur; }
    return cur;
}

static int64_t stream_read(exfat_fs_t *fs, uint32_t first_cluster, bool no_fat_chain,
                            uint32_t *cache_index, uint32_t *cache_cluster,
                            uint64_t pos, void *out_buf, uint64_t size) {
    uint8_t *scratch = kmalloc(fs->cluster_size);
    if (!scratch) return FS_ERR_NOMEM;

    uint8_t *out = (uint8_t *)out_buf;
    uint64_t done = 0;
    while (done < size) {
        uint32_t cidx = (uint32_t)((pos + done) / fs->cluster_size);
        uint32_t off = (uint32_t)((pos + done) % fs->cluster_size);

        uint32_t phys = cluster_for_index(fs, first_cluster, no_fat_chain, cache_index, cache_cluster, cidx);
        if (phys == 0) break;

        uint64_t chunk = fs->cluster_size - off;
        if (chunk > size - done) chunk = size - done;

        if (read_cluster(fs, phys, scratch) != FS_OK) break;
        memcpy(out + done, scratch + off, (size_t)chunk);
        done += chunk;
    }

    kfree(scratch);
    return (int64_t)done;
}

static int64_t stream_write(exfat_fs_t *fs, uint32_t first_cluster, bool no_fat_chain,
                             uint32_t *cache_index, uint32_t *cache_cluster,
                             uint64_t pos, const void *in_buf, uint64_t size) {
    uint8_t *scratch = kmalloc(fs->cluster_size);
    if (!scratch) return FS_ERR_NOMEM;

    const uint8_t *in = (const uint8_t *)in_buf;
    uint64_t done = 0;
    while (done < size) {
        uint32_t cidx = (uint32_t)((pos + done) / fs->cluster_size);
        uint32_t off = (uint32_t)((pos + done) % fs->cluster_size);

        uint32_t phys = cluster_for_index(fs, first_cluster, no_fat_chain, cache_index, cache_cluster, cidx);
        if (phys == 0) break;

        uint64_t chunk = fs->cluster_size - off;
        if (chunk > size - done) chunk = size - done;

        if (chunk != fs->cluster_size) {
            if (read_cluster(fs, phys, scratch) != FS_OK) break;
        }
        if (in) memcpy(scratch + off, in + done, (size_t)chunk);
        else memset(scratch + off, 0, (size_t)chunk);
        if (write_cluster(fs, phys, scratch) != FS_OK) break;
        done += chunk;
    }

    kfree(scratch);
    return (int64_t)done;
}

static inline bool bitmap_test(exfat_fs_t *fs, uint32_t cluster) {
    uint32_t i = cluster - 2;
    return (fs->bitmap[i >> 3] & (1u << (i & 7u))) != 0;
}

static int bitmap_flush_byte(exfat_fs_t *fs, uint32_t byte_index) {
    uint64_t sector_off = ((uint64_t)byte_index / fs->bytes_per_sector) * fs->bytes_per_sector;
    uint32_t cidx = (uint32_t)(sector_off / fs->cluster_size);
    uint32_t phys = cluster_for_index(fs, fs->bitmap_cluster, false, NULL, NULL, cidx);
    if (!phys) return FS_ERR_IO;
    uint64_t lba = cluster_to_lba(fs, phys) + (sector_off % fs->cluster_size) / fs->bytes_per_sector;
    if (fs->dev->write_sectors(fs->dev, lba, 1, fs->bitmap + sector_off) != 0) {
        LOG_ERROR("%s: failed to write allocation bitmap", fs->dev->name);
        return FS_ERR_IO;
    }
    return FS_OK;
}

static int bitmap_set(exfat_fs_t *fs, uint32_t cluster, bool used) {
    if (!cluster_valid(fs, cluster)) return FS_ERR_PARAM;
    uint32_t i = cluster - 2;
    uint8_t mask = (uint8_t)(1u << (i & 7u));
    bool was = (fs->bitmap[i >> 3] & mask) != 0;
    if (was == used) return FS_OK;
    if (used) { fs->bitmap[i >> 3] |= mask; fs->free_clusters--; }
    else { fs->bitmap[i >> 3] &= (uint8_t)~mask; fs->free_clusters++; }
    return bitmap_flush_byte(fs, i >> 3);
}

static uint32_t alloc_cluster(exfat_fs_t *fs, uint32_t want) {
    if (cluster_valid(fs, want) && !bitmap_test(fs, want)) {
        if (bitmap_set(fs, want, true) != FS_OK) return 0;
        return want;
    }
    if (fs->free_clusters == 0) return 0;

    uint32_t start = cluster_valid(fs, fs->alloc_hint) ? fs->alloc_hint - 2 : 0;
    for (uint32_t scanned = 0; scanned < fs->cluster_count; ) {
        uint32_t i = (start + scanned) % fs->cluster_count;
        if ((i & 7u) == 0 && i + 8 <= fs->cluster_count && fs->bitmap[i >> 3] == 0xFF) {
            scanned += 8;
            continue;
        }
        uint32_t c = i + 2;
        if (!bitmap_test(fs, c)) {
            if (bitmap_set(fs, c, true) != FS_OK) return 0;
            fs->alloc_hint = c + 1;
            return c;
        }
        scanned++;
    }
    return 0;
}

static int chain_find_last(exfat_fs_t *fs, exfat_chain_t *chain) {
    if (chain->count == 0 || chain->first_cluster < 2) { chain->last_cluster = 0; return FS_OK; }
    if (chain->no_fat_chain) { chain->last_cluster = chain->first_cluster + chain->count - 1; return FS_OK; }
    uint32_t c = cluster_for_index(fs, chain->first_cluster, false, NULL, NULL, chain->count - 1);
    if (!c) return FS_ERR_CORRUPT;
    chain->last_cluster = c;
    return FS_OK;
}

static int chain_make_fat(exfat_fs_t *fs, exfat_chain_t *chain) {
    for (uint32_t i = 0; i < chain->count; i++) {
        uint32_t c = chain->first_cluster + i;
        uint32_t next = (i + 1 < chain->count) ? c + 1 : EXFAT_CLUSTER_EOC;
        if (set_fat_entry(fs, c, next) != FS_OK) return FS_ERR_IO;
    }
    chain->no_fat_chain = false;
    return FS_OK;
}

static int chain_extend(exfat_fs_t *fs, exfat_chain_t *chain, uint32_t new_count) {
    if (chain->count > 0 && chain->last_cluster < 2) {
        int r = chain_find_last(fs, chain);
        if (r != FS_OK) return r;
    }

    while (chain->count < new_count) {
        if (chain->count == 0) {
            uint32_t c = alloc_cluster(fs, 0);
            if (!c) return FS_ERR_NOSPACE;
            chain->first_cluster = c;
            chain->last_cluster = c;
            chain->no_fat_chain = true;
            chain->count = 1;
            continue;
        }

        uint32_t want = chain->last_cluster + 1;
        uint32_t c = alloc_cluster(fs, want);
        if (!c) return FS_ERR_NOSPACE;

        if (c == want && chain->no_fat_chain) {
            chain->last_cluster = c;
            chain->count++;
            continue;
        }

        if (chain->no_fat_chain) {
            int r = chain_make_fat(fs, chain);
            if (r != FS_OK) { bitmap_set(fs, c, false); return r; }
        }
        if (set_fat_entry(fs, chain->last_cluster, c) != FS_OK) return FS_ERR_IO;
        if (set_fat_entry(fs, c, EXFAT_CLUSTER_EOC) != FS_OK) return FS_ERR_IO;
        chain->last_cluster = c;
        chain->count++;
    }
    return FS_OK;
}

static int chain_shrink(exfat_fs_t *fs, exfat_chain_t *chain, uint32_t new_count) {
    if (new_count >= chain->count) return FS_OK;

    if (chain->no_fat_chain) {
        for (uint32_t i = new_count; i < chain->count; i++)
            bitmap_set(fs, chain->first_cluster + i, false);
    } else {
        uint32_t cur;
        if (new_count == 0) {
            cur = chain->first_cluster;
        } else {
            uint32_t keep_last = cluster_for_index(fs, chain->first_cluster, false, NULL, NULL, new_count - 1);
            if (!keep_last) return FS_ERR_CORRUPT;
            cur = get_fat_entry(fs, keep_last);
            if (set_fat_entry(fs, keep_last, EXFAT_CLUSTER_EOC) != FS_OK) return FS_ERR_IO;
            chain->last_cluster = keep_last;
        }
        for (uint32_t i = new_count; i < chain->count && cluster_valid(fs, cur); i++) {
            uint32_t next = get_fat_entry(fs, cur);
            bitmap_set(fs, cur, false);
            cur = next;
        }
    }

    chain->count = new_count;
    if (new_count == 0) {
        chain->first_cluster = 0;
        chain->last_cluster = 0;
        chain->no_fat_chain = false;
    } else if (chain->no_fat_chain) {
        chain->last_cluster = chain->first_cluster + new_count - 1;
    }
    return FS_OK;
}

static void utf16_to_utf8(const uint16_t *src, uint32_t src_len, char *dst, size_t dst_cap) {
    size_t o = 0;
    for (uint32_t i = 0; i < src_len && o + 4 < dst_cap; i++) {
        uint32_t c = src[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < src_len && src[i + 1] >= 0xDC00 && src[i + 1] <= 0xDFFF) {
            c = 0x10000u + ((c - 0xD800u) << 10) + (src[i + 1] - 0xDC00u);
            i++;
        }
        if (c < 0x80) {
            dst[o++] = (char)c;
        } else if (c < 0x800) {
            dst[o++] = (char)(0xC0 | (c >> 6));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            dst[o++] = (char)(0xE0 | (c >> 12));
            dst[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            dst[o++] = (char)(0xF0 | (c >> 18));
            dst[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
            dst[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    dst[o] = '\0';
}

static int utf8_to_utf16(const char *src, uint16_t *out, uint32_t max) {
    const uint8_t *p = (const uint8_t *)src;
    uint32_t n = 0;
    while (*p) {
        uint32_t cp;
        if (p[0] < 0x80) {
            cp = p[0]; p += 1;
        } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2;
        } else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3;
        } else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                 ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F); p += 4;
        } else {
            return -1;
        }
        if (cp >= 0x10000) {
            if (n + 2 > max) return -1;
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xD800 + (cp >> 10));
            out[n++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            if (n + 1 > max) return -1;
            out[n++] = (uint16_t)cp;
        }
    }
    return (int)n;
}

static bool name_chars_valid(const uint16_t *name, uint32_t len) {
    if (len == 0 || len > 255) return false;
    for (uint32_t i = 0; i < len; i++) {
        uint16_t c = name[i];
        if (c < 0x20) return false;
        switch (c) {
            case '"': case '*': case '/': case ':': case '<': case '>': case '?': case '\\': case '|':
                return false;
            default:
                break;
        }
    }
    return true;
}

static inline uint16_t upcase_char(exfat_fs_t *fs, uint16_t c) {
    if (fs->upcase) return fs->upcase[c];
    return (c >= 'a' && c <= 'z') ? (uint16_t)(c - 32) : c;
}

static bool names_equal(exfat_fs_t *fs, const uint16_t *a, uint32_t alen, const uint16_t *b, uint32_t blen) {
    if (alen != blen) return false;
    for (uint32_t i = 0; i < alen; i++)
        if (upcase_char(fs, a[i]) != upcase_char(fs, b[i])) return false;
    return true;
}

static uint16_t name_hash(exfat_fs_t *fs, const uint16_t *name, uint32_t len) {
    uint16_t hash = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint16_t c = upcase_char(fs, name[i]);
        hash = (uint16_t)(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (c & 0xFF));
        hash = (uint16_t)(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (c >> 8));
    }
    return hash;
}

static uint16_t set_checksum(const uint8_t *buf, uint32_t entries) {
    uint16_t sum = 0;
    for (uint32_t i = 0; i < entries * 32; i++) {
        if (i == 2 || i == 3) continue;
        sum = (uint16_t)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + buf[i]);
    }
    return sum;
}

static uint32_t exfat_timestamp(uint8_t *ms10) {
    struct rtc_driver *rtc = get_self_driver(TIMER_DRIVER, RTC_TIMER);
    struct system_time *t = (rtc && rtc->get_rtc_time) ? rtc->get_rtc_time() : NULL;
    if (!t || t->year < 1980) {
        if (ms10) *ms10 = 0;
        return ((uint32_t)(2026 - 1980) << 25) | (1u << 21) | (1u << 16);
    }
    if (ms10) *ms10 = (uint8_t)((t->seconds & 1u) * 100u);
    return ((uint32_t)(t->year - 1980) << 25) | ((uint32_t)t->month << 21) | ((uint32_t)t->day << 16) |
           ((uint32_t)t->hours << 11) | ((uint32_t)t->minutes << 5) | ((uint32_t)t->seconds / 2u);
}

static void mark_dirty(exfat_fs_t *fs) {
    if (fs->dirty_marked) return;
    fs->dirty_marked = true;
    if (fs->was_dirty) return;

    uint8_t *buf = kmalloc(fs->bytes_per_sector);
    if (!buf) return;
    if (fs->dev->read_sectors(fs->dev, 0, 1, buf) == 0) {
        uint16_t flags;
        memcpy(&flags, buf + EXFAT_VOLUME_FLAGS_OFFSET, 2);
        flags |= EXFAT_VOLUME_FLAG_DIRTY;
        memcpy(buf + EXFAT_VOLUME_FLAGS_OFFSET, &flags, 2);
        if (fs->dev->write_sectors(fs->dev, 0, 1, buf) != 0)
            LOG_WARNING("%s: failed to set VolumeDirty", fs->dev->name);
    }
    kfree(buf);
}

static void clear_dirty(exfat_fs_t *fs) {
    if (!fs->dirty_marked) return;

    uint8_t *buf = kmalloc(fs->bytes_per_sector);
    if (!buf) return;
    if (fs->dev->read_sectors(fs->dev, 0, 1, buf) == 0) {
        uint16_t flags;
        memcpy(&flags, buf + EXFAT_VOLUME_FLAGS_OFFSET, 2);
        if (!fs->was_dirty) flags &= (uint16_t)~EXFAT_VOLUME_FLAG_DIRTY;
        memcpy(buf + EXFAT_VOLUME_FLAGS_OFFSET, &flags, 2);
        uint32_t used = fs->cluster_count - fs->free_clusters;
        buf[EXFAT_PERCENT_IN_USE_OFFSET] = fs->cluster_count
            ? (uint8_t)(((uint64_t)used * 100u) / fs->cluster_count) : 0;
        if (fs->dev->write_sectors(fs->dev, 0, 1, buf) != 0)
            LOG_WARNING("%s: failed to clear VolumeDirty", fs->dev->name);
    }
    kfree(buf);
    fs->dirty_marked = false;
}

static int writable(exfat_fs_t *fs) {
    if (!fs->bitmap || !fs->upcase) return FS_ERR_NOSUPP;
    mark_dirty(fs);
    return FS_OK;
}

static void root_dir(exfat_fs_t *fs, exfat_dir_t *d) {
    memset(d, 0, sizeof(*d));
    d->chain.first_cluster = fs->root_cluster;
    d->chain.no_fat_chain = false;
    d->chain.count = fs->root_clusters;
    d->chain.last_cluster = 0;
    d->size = (uint64_t)fs->root_clusters * fs->cluster_size;
    d->is_root = true;
}

static uint32_t clusters_for(exfat_fs_t *fs, uint64_t bytes) {
    return (uint32_t)((bytes + fs->cluster_size - 1) / fs->cluster_size);
}

static void dir_from_info(exfat_fs_t *fs, const exfat_entry_info_t *info, exfat_dir_t *d) {
    memset(d, 0, sizeof(*d));
    d->chain = info->chain;
    d->chain.count = clusters_for(fs, info->data_length);
    d->size = info->data_length;
    d->is_root = false;
    d->loc = info->loc;
}

static const uint8_t *cursor_entry(exfat_fs_t *fs, const exfat_dir_t *dir, exfat_cursor_t *cur, uint64_t pos) {
    if (pos + 32 > dir->size) return NULL;
    uint32_t idx = (uint32_t)(pos / fs->cluster_size);
    if (cur->loaded != idx) {
        uint32_t phys = cluster_for_index(fs, dir->chain.first_cluster, dir->chain.no_fat_chain,
                                          &cur->cache_index, &cur->cache_cluster, idx);
        if (!phys || read_cluster(fs, phys, cur->buf) != FS_OK) { cur->loaded = UINT32_MAX; return NULL; }
        cur->loaded = idx;
    }
    return cur->buf + (pos % fs->cluster_size);
}

static int cursor_open(exfat_fs_t *fs, const exfat_dir_t *dir, exfat_cursor_t *cur) {
    cur->buf = kmalloc(fs->cluster_size);
    if (!cur->buf) return FS_ERR_NOMEM;
    cur->loaded = UINT32_MAX;
    cur->cache_index = 0;
    cur->cache_cluster = dir->chain.first_cluster;
    return FS_OK;
}

static void cursor_close(exfat_cursor_t *cur) {
    if (cur->buf) kfree(cur->buf);
    cur->buf = NULL;
}

static int parse_set(exfat_fs_t *fs, const exfat_dir_t *dir, exfat_cursor_t *cur, uint64_t pos,
                     exfat_entry_info_t *out, uint16_t *name16, uint32_t *name_len) {
    const uint8_t *e = cursor_entry(fs, dir, cur, pos);
    if (!e || e[0] != EXFAT_ENTRY_TYPE_FILE) return FS_ERR_CORRUPT;

    exfat_file_entry_t fe;
    memcpy(&fe, e, sizeof(fe));
    if (fe.secondary_count < 2 || fe.secondary_count > EXFAT_MAX_SET_ENTRIES - 1) return FS_ERR_CORRUPT;

    e = cursor_entry(fs, dir, cur, pos + 32);
    if (!e || e[0] != EXFAT_ENTRY_TYPE_STREAM) return FS_ERR_CORRUPT;
    exfat_stream_entry_t se;
    memcpy(&se, e, sizeof(se));

    uint32_t got = 0;
    for (uint32_t k = 2; k <= fe.secondary_count && got < se.name_length; k++) {
        e = cursor_entry(fs, dir, cur, pos + (uint64_t)k * 32);
        if (!e) return FS_ERR_CORRUPT;
        if (e[0] != EXFAT_ENTRY_TYPE_FILENAME) continue;
        exfat_filename_entry_t ne;
        memcpy(&ne, e, sizeof(ne));
        for (int c = 0; c < 15 && got < se.name_length; c++) name16[got++] = ne.name[c];
    }

    out->attributes = fe.file_attributes;
    out->chain.first_cluster = se.first_cluster;
    out->chain.no_fat_chain = (se.general_flags & EXFAT_STREAM_FLAG_NOFATCHAIN) != 0;
    out->chain.count = clusters_for(fs, se.data_length);
    out->chain.last_cluster = 0;
    if (out->chain.first_cluster < 2) out->chain.count = 0;
    out->data_length = se.data_length;
    out->valid_data_length = se.valid_data_length;
    out->loc.dir_first_cluster = dir->chain.first_cluster;
    out->loc.dir_no_fat_chain = dir->chain.no_fat_chain;
    out->loc.dir_size = dir->size;
    out->loc.set_pos = pos;
    out->loc.set_count = (uint8_t)(fe.secondary_count + 1);
    utf16_to_utf8(name16, got, out->name_utf8, sizeof(out->name_utf8));
    *name_len = got;
    return FS_OK;
}

static int dir_find(exfat_fs_t *fs, const exfat_dir_t *dir, const uint16_t *name, uint32_t len,
                    exfat_entry_info_t *out) {
    exfat_cursor_t cur;
    if (cursor_open(fs, dir, &cur) != FS_OK) return FS_ERR_NOMEM;

    uint16_t namebuf[256];
    int ret = FS_ERR_NOENT;
    uint64_t pos = 0;
    while (pos < dir->size) {
        const uint8_t *e = cursor_entry(fs, dir, &cur, pos);
        if (!e) { ret = FS_ERR_IO; break; }
        if (e[0] == EXFAT_ENTRY_EOD) break;
        if (e[0] != EXFAT_ENTRY_TYPE_FILE) { pos += 32; continue; }

        uint32_t got = 0;
        if (parse_set(fs, dir, &cur, pos, out, namebuf, &got) != FS_OK) { pos += 32; continue; }
        if (names_equal(fs, namebuf, got, name, len)) { ret = FS_OK; break; }
        pos += (uint64_t)out->loc.set_count * 32;
    }

    cursor_close(&cur);
    return ret;
}

static int dir_is_empty(exfat_fs_t *fs, const exfat_dir_t *dir, bool *empty) {
    exfat_cursor_t cur;
    if (cursor_open(fs, dir, &cur) != FS_OK) return FS_ERR_NOMEM;
    *empty = true;
    int ret = FS_OK;
    for (uint64_t pos = 0; pos < dir->size; pos += 32) {
        const uint8_t *e = cursor_entry(fs, dir, &cur, pos);
        if (!e) { ret = FS_ERR_IO; break; }
        if (e[0] == EXFAT_ENTRY_EOD) break;
        if (e[0] == EXFAT_ENTRY_TYPE_FILE) { *empty = false; break; }
    }
    cursor_close(&cur);
    return ret;
}

static int to_name16(const char *utf8, uint16_t *out, uint32_t *len) {
    int n = utf8_to_utf16(utf8, out, 255);
    if (n <= 0) return FS_ERR_PARAM;
    *len = (uint32_t)n;
    return FS_OK;
}

static int resolve_dir(exfat_fs_t *fs, const char *path, exfat_dir_t *out) {
    fs_path_parts_t pp;
    int r = fs_split_path(path, &pp);
    if (r != FS_OK) return r;

    exfat_dir_t cur;
    root_dir(fs, &cur);
    for (int i = 0; i < pp.count; i++) {
        uint16_t name16[256]; uint32_t len;
        r = to_name16(pp.comps[i], name16, &len);
        if (r != FS_OK) return r;
        exfat_entry_info_t info;
        r = dir_find(fs, &cur, name16, len, &info);
        if (r != FS_OK) return r;
        if (!(info.attributes & EXFAT_ATTR_DIRECTORY)) return FS_ERR_NOTDIR;
        dir_from_info(fs, &info, &cur);
    }
    *out = cur;
    return FS_OK;
}

static int resolve_parent(exfat_fs_t *fs, const char *path, exfat_dir_t *parent,
                          uint16_t *leaf16, uint32_t *leaf_len) {
    fs_path_parts_t pp;
    int r = fs_split_path(path, &pp);
    if (r != FS_OK) return r;
    if (pp.count == 0) return FS_ERR_PARAM;

    exfat_dir_t cur;
    root_dir(fs, &cur);
    for (int i = 0; i < pp.count - 1; i++) {
        uint16_t name16[256]; uint32_t len;
        r = to_name16(pp.comps[i], name16, &len);
        if (r != FS_OK) return r;
        exfat_entry_info_t info;
        r = dir_find(fs, &cur, name16, len, &info);
        if (r != FS_OK) return r;
        if (!(info.attributes & EXFAT_ATTR_DIRECTORY)) return FS_ERR_NOTDIR;
        dir_from_info(fs, &info, &cur);
    }

    r = to_name16(pp.comps[pp.count - 1], leaf16, leaf_len);
    if (r != FS_OK) return r;
    *parent = cur;
    return FS_OK;
}

static int resolve_entry(exfat_fs_t *fs, const char *path, exfat_entry_info_t *out, bool *is_root) {
    fs_path_parts_t pp;
    int r = fs_split_path(path, &pp);
    if (r != FS_OK) return r;
    if (pp.count == 0) {
        *is_root = true;
        memset(out, 0, sizeof(*out));
        out->attributes = EXFAT_ATTR_DIRECTORY;
        out->chain.first_cluster = fs->root_cluster;
        out->chain.count = fs->root_clusters;
        return FS_OK;
    }
    *is_root = false;

    exfat_dir_t parent;
    uint16_t leaf16[256]; uint32_t leaf_len;
    r = resolve_parent(fs, path, &parent, leaf16, &leaf_len);
    if (r != FS_OK) return r;
    return dir_find(fs, &parent, leaf16, leaf_len, out);
}

static int set_read(exfat_fs_t *fs, const exfat_loc_t *loc, uint8_t *buf) {
    uint64_t len = (uint64_t)loc->set_count * 32;
    int64_t n = stream_read(fs, loc->dir_first_cluster, loc->dir_no_fat_chain, NULL, NULL,
                            loc->set_pos, buf, len);
    return n == (int64_t)len ? FS_OK : FS_ERR_IO;
}

static int set_write(exfat_fs_t *fs, const exfat_loc_t *loc, uint8_t *buf, bool checksum) {
    if (checksum) {
        uint16_t sum = set_checksum(buf, loc->set_count);
        memcpy(buf + 2, &sum, 2);
    }
    uint64_t len = (uint64_t)loc->set_count * 32;
    int64_t n = stream_write(fs, loc->dir_first_cluster, loc->dir_no_fat_chain, NULL, NULL,
                             loc->set_pos, buf, len);
    return n == (int64_t)len ? FS_OK : FS_ERR_IO;
}

static int update_set_stream(exfat_fs_t *fs, const exfat_loc_t *loc, const exfat_chain_t *chain,
                             uint64_t size, uint64_t valid_size) {
    uint8_t buf[EXFAT_MAX_SET_ENTRIES * 32];
    if (loc->set_count < 3 || loc->set_count > EXFAT_MAX_SET_ENTRIES) return FS_ERR_CORRUPT;
    int r = set_read(fs, loc, buf);
    if (r != FS_OK) return r;
    if (buf[0] != EXFAT_ENTRY_TYPE_FILE || buf[32] != EXFAT_ENTRY_TYPE_STREAM) return FS_ERR_CORRUPT;

    exfat_file_entry_t fe;
    exfat_stream_entry_t se;
    memcpy(&fe, buf, sizeof(fe));
    memcpy(&se, buf + 32, sizeof(se));

    uint8_t ms10;
    fe.modified_timestamp = exfat_timestamp(&ms10);
    fe.modified_10ms = ms10;
    fe.accessed_timestamp = fe.modified_timestamp;
    if (!(fe.file_attributes & EXFAT_ATTR_DIRECTORY)) fe.file_attributes |= EXFAT_ATTR_ARCHIVE;

    se.general_flags = EXFAT_STREAM_FLAG_ALLOC;
    if (chain->count > 0 && chain->no_fat_chain) se.general_flags |= EXFAT_STREAM_FLAG_NOFATCHAIN;
    se.first_cluster = chain->count > 0 ? chain->first_cluster : 0;
    se.data_length = size;
    se.valid_data_length = valid_size;

    memcpy(buf, &fe, sizeof(fe));
    memcpy(buf + 32, &se, sizeof(se));
    return set_write(fs, loc, buf, true);
}

static int dir_extend(exfat_fs_t *fs, exfat_dir_t *dir) {
    uint32_t old_count = dir->chain.count;
    int r = chain_extend(fs, &dir->chain, old_count + 1);
    if (r != FS_OK) return r;
    r = zero_cluster(fs, dir->chain.last_cluster);
    if (r != FS_OK) return r;
    dir->size += fs->cluster_size;

    if (dir->is_root) {
        fs->root_clusters = dir->chain.count;
        return FS_OK;
    }
    return update_set_stream(fs, &dir->loc, &dir->chain, dir->size, dir->size);
}

static int create_set(exfat_fs_t *fs, exfat_dir_t *dir, const uint16_t *name16, uint32_t name_len,
                      uint16_t attributes, const exfat_chain_t *chain, uint64_t size, exfat_loc_t *out) {
    uint32_t name_entries = (name_len + 14) / 15;
    uint32_t count = 2 + name_entries;
    if (name_entries > EXFAT_MAX_NAME_ENTRIES) return FS_ERR_PARAM;

    exfat_cursor_t cur;
    if (cursor_open(fs, dir, &cur) != FS_OK) return FS_ERR_NOMEM;

    uint64_t run_start = 0;
    uint32_t run_len = 0;
    bool found = false;
    for (uint64_t pos = 0; pos < dir->size; pos += 32) {
        const uint8_t *e = cursor_entry(fs, dir, &cur, pos);
        if (!e) { cursor_close(&cur); return FS_ERR_IO; }
        if ((e[0] & EXFAT_ENTRY_INUSE_BIT) == 0) {
            if (run_len == 0) run_start = pos;
            run_len++;
            if (run_len >= count) { found = true; break; }
        } else {
            run_len = 0;
        }
    }
    cursor_close(&cur);

    while (!found) {
        uint64_t old_size = dir->size;
        int r = dir_extend(fs, dir);
        if (r != FS_OK) return r;
        if (run_len == 0) run_start = old_size;
        run_len += fs->cluster_size / 32;
        if (run_len >= count) found = true;
    }

    uint8_t buf[EXFAT_MAX_SET_ENTRIES * 32];
    memset(buf, 0, sizeof(buf));

    exfat_file_entry_t fe;
    memset(&fe, 0, sizeof(fe));
    fe.entry_type = EXFAT_ENTRY_TYPE_FILE;
    fe.secondary_count = (uint8_t)(count - 1);
    fe.file_attributes = attributes;
    uint8_t ms10;
    fe.create_timestamp = exfat_timestamp(&ms10);
    fe.modified_timestamp = fe.create_timestamp;
    fe.accessed_timestamp = fe.create_timestamp;
    fe.create_10ms = ms10;
    fe.modified_10ms = ms10;
    memcpy(buf, &fe, sizeof(fe));

    exfat_stream_entry_t se;
    memset(&se, 0, sizeof(se));
    se.entry_type = EXFAT_ENTRY_TYPE_STREAM;
    se.general_flags = EXFAT_STREAM_FLAG_ALLOC;
    if (chain->count > 0 && chain->no_fat_chain) se.general_flags |= EXFAT_STREAM_FLAG_NOFATCHAIN;
    se.name_length = (uint8_t)name_len;
    se.name_hash = name_hash(fs, name16, name_len);
    se.valid_data_length = size;
    se.first_cluster = chain->count > 0 ? chain->first_cluster : 0;
    se.data_length = size;
    memcpy(buf + 32, &se, sizeof(se));

    for (uint32_t k = 0; k < name_entries; k++) {
        exfat_filename_entry_t ne;
        memset(&ne, 0, sizeof(ne));
        ne.entry_type = EXFAT_ENTRY_TYPE_FILENAME;
        for (uint32_t c = 0; c < 15; c++) {
            uint32_t idx = k * 15 + c;
            ne.name[c] = idx < name_len ? name16[idx] : 0;
        }
        memcpy(buf + 64 + k * 32, &ne, sizeof(ne));
    }

    exfat_loc_t loc;
    loc.dir_first_cluster = dir->chain.first_cluster;
    loc.dir_no_fat_chain = dir->chain.no_fat_chain;
    loc.dir_size = dir->size;
    loc.set_pos = run_start;
    loc.set_count = (uint8_t)count;

    int r = set_write(fs, &loc, buf, true);
    if (r != FS_OK) return r;
    if (out) *out = loc;
    return FS_OK;
}

static int delete_set(exfat_fs_t *fs, const exfat_loc_t *loc) {
    uint8_t buf[EXFAT_MAX_SET_ENTRIES * 32];
    if (loc->set_count < 1 || loc->set_count > EXFAT_MAX_SET_ENTRIES) return FS_ERR_CORRUPT;
    int r = set_read(fs, loc, buf);
    if (r != FS_OK) return r;
    for (uint32_t i = 0; i < loc->set_count; i++) buf[i * 32] &= (uint8_t)~EXFAT_ENTRY_INUSE_BIT;
    return set_write(fs, loc, buf, false);
}

static int64_t file_write_range(exfat_file_t *ff, uint64_t pos, const uint8_t *in, uint64_t len) {
    exfat_fs_t *fs = ff->fs;
    if (len == 0) return 0;

    uint32_t needed = clusters_for(fs, pos + len);
    if (needed > ff->chain.count) {
        int r = chain_extend(fs, &ff->chain, needed);
        if (r != FS_OK) return r;
        ff->dirty = true;
    }

    uint64_t done = 0;
    while (done < len) {
        uint32_t cidx = (uint32_t)((pos + done) / fs->cluster_size);
        uint32_t off = (uint32_t)((pos + done) % fs->cluster_size);
        uint32_t phys = cluster_for_index(fs, ff->chain.first_cluster, ff->chain.no_fat_chain,
                                          &ff->cache_index, &ff->cache_cluster, cidx);
        if (phys == 0) break;

        uint64_t chunk = fs->cluster_size - off;
        if (chunk > len - done) chunk = len - done;

        if (chunk != fs->cluster_size) {
            if (read_cluster(fs, phys, ff->iobuf) != FS_OK) break;
        }
        if (in) memcpy(ff->iobuf + off, in + done, (size_t)chunk);
        else memset(ff->iobuf + off, 0, (size_t)chunk);
        if (write_cluster(fs, phys, ff->iobuf) != FS_OK) break;
        done += chunk;
    }
    return (int64_t)done;
}

static int file_fill_valid(exfat_file_t *ff, uint64_t upto) {
    if (ff->valid_size >= upto) return FS_OK;
    uint64_t len = upto - ff->valid_size;
    int64_t n = file_write_range(ff, ff->valid_size, NULL, len);
    if (n != (int64_t)len) return n < 0 ? (int)n : FS_ERR_IO;
    ff->valid_size = upto;
    ff->dirty = true;
    return FS_OK;
}

static void file_reset_cache(exfat_file_t *ff) {
    ff->cache_index = 0;
    ff->cache_cluster = ff->chain.first_cluster;
}

static int exfat_op_open(fs_t *fsroot, const char *path, bool create, bool truncate_flag, fs_file_t *out) {
    exfat_fs_t *fs = (exfat_fs_t *)fsroot->priv;

    exfat_dir_t parent;
    uint16_t leaf16[256]; uint32_t leaf_len;
    int r = resolve_parent(fs, path, &parent, leaf16, &leaf_len);
    if (r != FS_OK) return r;

    exfat_entry_info_t info;
    r = dir_find(fs, &parent, leaf16, leaf_len, &info);
    if (r == FS_OK) {
        if (info.attributes & EXFAT_ATTR_DIRECTORY) return FS_ERR_ISDIR;
    } else if (r == FS_ERR_NOENT && create) {
        if (!name_chars_valid(leaf16, leaf_len)) return FS_ERR_PARAM;
        r = writable(fs);
        if (r != FS_OK) return r;
        memset(&info, 0, sizeof(info));
        info.attributes = EXFAT_ATTR_ARCHIVE;
        r = create_set(fs, &parent, leaf16, leaf_len, info.attributes, &info.chain, 0, &info.loc);
        if (r != FS_OK) return r;
    } else {
        return r;
    }

    exfat_file_t *ef = kmalloc(sizeof(exfat_file_t));
    if (!ef) return FS_ERR_NOMEM;
    memset(ef, 0, sizeof(*ef));
    ef->fs = fs;
    ef->chain = info.chain;
    ef->size = info.data_length;
    ef->valid_size = info.valid_data_length < info.data_length ? info.valid_data_length : info.data_length;
    ef->loc = info.loc;
    ef->attributes = info.attributes;
    file_reset_cache(ef);
    ef->iobuf = kmalloc(fs->cluster_size);
    if (!ef->iobuf) { kfree(ef); return FS_ERR_NOMEM; }

    if (truncate_flag && (ef->size || ef->chain.count)) {
        r = writable(fs);
        if (r == FS_OK) r = chain_shrink(fs, &ef->chain, 0);
        if (r == FS_OK) {
            ef->size = 0;
            ef->valid_size = 0;
            file_reset_cache(ef);
            r = update_set_stream(fs, &ef->loc, &ef->chain, 0, 0);
        }
        if (r != FS_OK) { kfree(ef->iobuf); kfree(ef); return r; }
    }

    out->priv = ef;
    out->size = ef->size;
    out->pos = 0;
    out->writable = fs->bitmap != NULL;
    return FS_OK;
}

static int exfat_op_close(fs_file_t *f) {
    exfat_file_t *ef = (exfat_file_t *)f->priv;
    if (!ef) return FS_OK;
    int r = FS_OK;
    if (ef->dirty) r = update_set_stream(ef->fs, &ef->loc, &ef->chain, ef->size, ef->valid_size);
    if (ef->iobuf) kfree(ef->iobuf);
    kfree(ef);
    f->priv = NULL;
    return r;
}

static int64_t exfat_op_read(fs_file_t *f, void *buf, uint64_t size) {
    exfat_file_t *ef = (exfat_file_t *)f->priv;
    if (f->pos >= ef->size) return 0;
    uint64_t remaining = ef->size - f->pos;
    if (size > remaining) size = remaining;

    uint8_t *out = (uint8_t *)buf;
    uint64_t done = 0;
    if (f->pos < ef->valid_size) {
        uint64_t real = ef->valid_size - f->pos;
        if (real > size) real = size;
        int64_t n = stream_read(ef->fs, ef->chain.first_cluster, ef->chain.no_fat_chain,
                                &ef->cache_index, &ef->cache_cluster, f->pos, out, real);
        if (n < 0) return n;
        done = (uint64_t)n;
        if (done != real) { f->pos += done; return (int64_t)done; }
    }
    if (done < size) memset(out + done, 0, (size_t)(size - done));
    f->pos += size;
    return (int64_t)size;
}

static int64_t exfat_op_write(fs_file_t *f, const void *buf, uint64_t size) {
    exfat_file_t *ef = (exfat_file_t *)f->priv;
    if (size == 0) return 0;
    int r = writable(ef->fs);
    if (r != FS_OK) return r;

    r = file_fill_valid(ef, ef->size);
    if (r != FS_OK) return r;
    if (f->pos > ef->size) {
        r = file_fill_valid(ef, f->pos);
        if (r != FS_OK) return r;
        ef->size = f->pos;
    }

    int64_t n = file_write_range(ef, f->pos, (const uint8_t *)buf, size);
    if (n <= 0) return n == 0 ? FS_ERR_NOSPACE : n;
    f->pos += (uint64_t)n;
    if (f->pos > ef->size) ef->size = f->pos;
    if (f->pos > ef->valid_size) ef->valid_size = f->pos;
    ef->dirty = true;
    f->size = ef->size;
    return n;
}

static int exfat_op_seek(fs_file_t *f, uint64_t pos) { f->pos = pos; return FS_OK; }

static int exfat_op_truncate(fs_file_t *f, uint64_t size) {
    exfat_file_t *ef = (exfat_file_t *)f->priv;
    exfat_fs_t *fs = ef->fs;
    int r = writable(fs);
    if (r != FS_OK) return r;

    if (size < ef->size) {
        r = chain_shrink(fs, &ef->chain, clusters_for(fs, size));
        if (r != FS_OK) return r;
        file_reset_cache(ef);
        ef->size = size;
        if (ef->valid_size > size) ef->valid_size = size;
    } else if (size > ef->size) {
        r = file_fill_valid(ef, size);
        if (r != FS_OK) return r;
        ef->size = size;
    }

    f->size = ef->size;
    if (f->pos > ef->size) f->pos = ef->size;
    r = update_set_stream(fs, &ef->loc, &ef->chain, ef->size, ef->valid_size);
    if (r == FS_OK) ef->dirty = false;
    return r;
}

static int exfat_op_opendir(fs_t *fsroot, const char *path, fs_dir_t *out) {
    exfat_fs_t *fs = (exfat_fs_t *)fsroot->priv;

    exfat_dir_t dir;
    int r = resolve_dir(fs, path, &dir);
    if (r != FS_OK) return r;

    exfat_diriter_t *it = kmalloc(sizeof(exfat_diriter_t));
    if (!it) return FS_ERR_NOMEM;
    memset(it, 0, sizeof(*it));
    it->fs = fs;
    it->dir = dir;
    it->pos = 0;
    it->cluster_buf = kmalloc(fs->cluster_size);
    if (!it->cluster_buf) { kfree(it); return FS_ERR_NOMEM; }
    it->loaded_index = UINT32_MAX;
    it->cache_index = 0;
    it->cache_cluster = dir.chain.first_cluster;

    out->priv = it;
    return FS_OK;
}

static int exfat_op_readdir(fs_dir_t *dir, fs_dirent_t *out) {
    exfat_diriter_t *it = (exfat_diriter_t *)dir->priv;
    exfat_fs_t *fs = it->fs;

    exfat_cursor_t cur;
    cur.buf = it->cluster_buf;
    cur.loaded = it->loaded_index;
    cur.cache_index = it->cache_index;
    cur.cache_cluster = it->cache_cluster;

    int ret = FS_ERR_EOF;
    uint16_t namebuf[256];
    while (it->pos < it->dir.size) {
        const uint8_t *e = cursor_entry(fs, &it->dir, &cur, it->pos);
        if (!e) { ret = FS_ERR_IO; break; }
        if (e[0] == EXFAT_ENTRY_EOD) break;
        if (e[0] != EXFAT_ENTRY_TYPE_FILE) { it->pos += 32; continue; }

        exfat_entry_info_t info;
        uint32_t got = 0;
        if (parse_set(fs, &it->dir, &cur, it->pos, &info, namebuf, &got) != FS_OK) { it->pos += 32; continue; }
        it->pos += (uint64_t)info.loc.set_count * 32;

        strncpy(out->name, info.name_utf8, FS_MAX_NAME);
        out->name[FS_MAX_NAME] = '\0';
        out->type = (info.attributes & EXFAT_ATTR_DIRECTORY) ? FS_ENTRY_DIR : FS_ENTRY_FILE;
        out->size = info.data_length;
        ret = FS_OK;
        break;
    }

    it->loaded_index = cur.loaded;
    it->cache_index = cur.cache_index;
    it->cache_cluster = cur.cache_cluster;
    return ret;
}

static int exfat_op_closedir(fs_dir_t *dir) {
    exfat_diriter_t *it = (exfat_diriter_t *)dir->priv;
    if (it) {
        if (it->cluster_buf) kfree(it->cluster_buf);
        kfree(it);
    }
    dir->priv = NULL;
    return FS_OK;
}

static int exfat_op_stat(fs_t *fsroot, const char *path, fs_dirent_t *out) {
    exfat_fs_t *fs = (exfat_fs_t *)fsroot->priv;

    fs_path_parts_t pp;
    int r = fs_split_path(path, &pp);
    if (r != FS_OK) return r;

    exfat_entry_info_t info;
    bool is_root;
    r = resolve_entry(fs, path, &info, &is_root);
    if (r != FS_OK) return r;

    const char *name = pp.count ? pp.comps[pp.count - 1] : "/";
    strncpy(out->name, name, FS_MAX_NAME); out->name[FS_MAX_NAME] = '\0';
    out->type = (info.attributes & EXFAT_ATTR_DIRECTORY) ? FS_ENTRY_DIR : FS_ENTRY_FILE;
    out->size = is_root ? 0 : info.data_length;
    return FS_OK;
}

static int exfat_op_mkdir(fs_t *fsroot, const char *path) {
    exfat_fs_t *fs = (exfat_fs_t *)fsroot->priv;

    exfat_dir_t parent;
    uint16_t leaf16[256]; uint32_t leaf_len;
    int r = resolve_parent(fs, path, &parent, leaf16, &leaf_len);
    if (r != FS_OK) return r;

    exfat_entry_info_t existing;
    r = dir_find(fs, &parent, leaf16, leaf_len, &existing);
    if (r == FS_OK) return FS_ERR_EXIST;
    if (r != FS_ERR_NOENT) return r;
    if (!name_chars_valid(leaf16, leaf_len)) return FS_ERR_PARAM;

    r = writable(fs);
    if (r != FS_OK) return r;

    exfat_chain_t chain;
    memset(&chain, 0, sizeof(chain));
    r = chain_extend(fs, &chain, 1);
    if (r != FS_OK) return r;
    r = zero_cluster(fs, chain.first_cluster);
    if (r == FS_OK)
        r = create_set(fs, &parent, leaf16, leaf_len, EXFAT_ATTR_DIRECTORY, &chain, fs->cluster_size, NULL);
    if (r != FS_OK) chain_shrink(fs, &chain, 0);
    return r;
}

static int remove_entry(exfat_fs_t *fs, const char *path, bool want_dir) {
    exfat_entry_info_t info;
    bool is_root;
    int r = resolve_entry(fs, path, &info, &is_root);
    if (r != FS_OK) return r;
    if (is_root) return FS_ERR_PARAM;

    bool is_dir = (info.attributes & EXFAT_ATTR_DIRECTORY) != 0;
    if (want_dir && !is_dir) return FS_ERR_NOTDIR;
    if (!want_dir && is_dir) return FS_ERR_ISDIR;

    if (is_dir) {
        exfat_dir_t d;
        dir_from_info(fs, &info, &d);
        bool empty;
        r = dir_is_empty(fs, &d, &empty);
        if (r != FS_OK) return r;
        if (!empty) return FS_ERR_NOTEMPTY;
    }

    r = writable(fs);
    if (r != FS_OK) return r;

    r = delete_set(fs, &info.loc);
    if (r != FS_OK) return r;
    return chain_shrink(fs, &info.chain, 0);
}

static int exfat_op_unlink(fs_t *fsroot, const char *path) {
    return remove_entry((exfat_fs_t *)fsroot->priv, path, false);
}

static int exfat_op_rmdir(fs_t *fsroot, const char *path) {
    return remove_entry((exfat_fs_t *)fsroot->priv, path, true);
}

static void exfat_free(exfat_fs_t *fs) {
    if (fs->bitmap) kfree(fs->bitmap);
    if (fs->upcase) kfree(fs->upcase);
    if (fs->fat_cache) kfree(fs->fat_cache);
    kfree(fs);
}

static int exfat_op_unmount(fs_t *fsroot) {
    exfat_fs_t *fs = (exfat_fs_t *)fsroot->priv;
    if (fs) {
        clear_dirty(fs);
        exfat_free(fs);
    }
    fsroot->priv = NULL;
    return FS_OK;
}

const fs_ops_t exfat_ops = {
    .open = exfat_op_open,
    .close = exfat_op_close,
    .read = exfat_op_read,
    .write = exfat_op_write,
    .seek = exfat_op_seek,
    .truncate = exfat_op_truncate,
    .opendir = exfat_op_opendir,
    .readdir = exfat_op_readdir,
    .closedir = exfat_op_closedir,
    .mkdir = exfat_op_mkdir,
    .unlink = exfat_op_unlink,
    .rmdir = exfat_op_rmdir,
    .stat = exfat_op_stat,
    .unmount = exfat_op_unmount,
};

static int load_bootsector(struct block_device *dev, exfat_bootsector_t *out) {
    uint32_t sector_size = dev->sector_size ? dev->sector_size : 512;
    uint8_t *buf = kmalloc(sector_size);
    if (!buf) return FS_ERR_NOMEM;
    if (dev->read_sectors(dev, 0, 1, buf) != 0) {
        LOG_ERROR("%s: failed to read boot sector", dev->name);
        kfree(buf);
        return FS_ERR_IO;
    }

    if (buf[EXFAT_BOOT_SIG_OFFSET] != EXFAT_BOOT_SIG_0 ||
        buf[EXFAT_BOOT_SIG_OFFSET + 1] != EXFAT_BOOT_SIG_1) {
        kfree(buf);
        return FS_ERR_CORRUPT;
    }

    memcpy(out, buf, sizeof(exfat_bootsector_t));
    kfree(buf);

    if (memcmp(out->fs_name, "EXFAT   ", 8) != 0) return FS_ERR_CORRUPT;
    return FS_OK;
}

int exfat_probe(struct block_device *dev) {
    if (!dev) return FS_ERR_PARAM;
    exfat_bootsector_t bs;
    return load_bootsector(dev, &bs);
}

static int load_system_entries(exfat_fs_t *fs, uint32_t *upcase_cluster, uint64_t *upcase_length) {
    exfat_dir_t root;
    root_dir(fs, &root);
    exfat_cursor_t cur;
    if (cursor_open(fs, &root, &cur) != FS_OK) return FS_ERR_NOMEM;

    fs->label[0] = '\0';
    *upcase_cluster = 0;
    *upcase_length = 0;
    for (uint64_t pos = 0; pos < root.size; pos += 32) {
        const uint8_t *e = cursor_entry(fs, &root, &cur, pos);
        if (!e || e[0] == EXFAT_ENTRY_EOD) break;
        if (e[0] == EXFAT_ENTRY_TYPE_LABEL) {
            uint8_t char_count = e[1];
            if (char_count > 11) char_count = 11;
            uint16_t namebuf[11];
            memcpy(namebuf, e + 2, (size_t)char_count * 2);
            utf16_to_utf8(namebuf, char_count, fs->label, sizeof(fs->label));
        } else if (e[0] == EXFAT_ENTRY_TYPE_BITMAP && !(e[1] & 1u) && !fs->bitmap_cluster) {
            memcpy(&fs->bitmap_cluster, e + 20, 4);
            memcpy(&fs->bitmap_length, e + 24, 8);
        } else if (e[0] == EXFAT_ENTRY_TYPE_UPCASE && !*upcase_cluster) {
            memcpy(upcase_cluster, e + 20, 4);
            memcpy(upcase_length, e + 24, 8);
        }
    }
    cursor_close(&cur);
    return FS_OK;
}

static void load_bitmap(exfat_fs_t *fs) {
    uint64_t need = ((uint64_t)fs->cluster_count + 7) / 8;
    if (!cluster_valid(fs, fs->bitmap_cluster) || fs->bitmap_length < need) {
        LOG_WARNING("%s: allocation bitmap missing or too small, mounting read-only", fs->dev->name);
        return;
    }
    uint32_t clusters = clusters_for(fs, fs->bitmap_length);
    uint8_t *bm = kmalloc((size_t)clusters * fs->cluster_size);
    if (!bm) return;
    memset(bm, 0, (size_t)clusters * fs->cluster_size);
    int64_t n = stream_read(fs, fs->bitmap_cluster, false, NULL, NULL, 0, bm, (uint64_t)clusters * fs->cluster_size);
    if (n != (int64_t)clusters * fs->cluster_size) {
        LOG_WARNING("%s: failed to read allocation bitmap, mounting read-only", fs->dev->name);
        kfree(bm);
        return;
    }
    fs->bitmap = bm;
    uint32_t used = 0;
    for (uint32_t i = 0; i < fs->cluster_count; i++)
        if (bm[i >> 3] & (1u << (i & 7u))) used++;
    fs->free_clusters = fs->cluster_count - used;
}

static void load_upcase(exfat_fs_t *fs, uint32_t cluster, uint64_t length) {
    if (!cluster_valid(fs, cluster) || length < 2 || length > EXFAT_UPCASE_ENTRIES * 2 * 2) {
        LOG_WARNING("%s: up-case table missing, mounting read-only", fs->dev->name);
        return;
    }
    uint16_t *table = kmalloc(EXFAT_UPCASE_ENTRIES * sizeof(uint16_t));
    uint16_t *raw = kmalloc((size_t)length);
    if (!table || !raw) {
        if (table) kfree(table);
        if (raw) kfree(raw);
        return;
    }
    if (stream_read(fs, cluster, false, NULL, NULL, 0, raw, length) != (int64_t)length) {
        kfree(table);
        kfree(raw);
        LOG_WARNING("%s: failed to read up-case table, mounting read-only", fs->dev->name);
        return;
    }

    for (uint32_t i = 0; i < EXFAT_UPCASE_ENTRIES; i++) table[i] = (uint16_t)i;
    uint32_t idx = 0;
    uint64_t count = length / 2;
    for (uint64_t i = 0; i < count && idx < EXFAT_UPCASE_ENTRIES; i++) {
        if (raw[i] == 0xFFFF && i + 1 < count) {
            idx += raw[++i];
            continue;
        }
        table[idx++] = raw[i];
    }
    kfree(raw);
    fs->upcase = table;
}

int exfat_mount(struct block_device *dev, fs_t *out) {
    if (!dev || !out) return FS_ERR_PARAM;

    exfat_bootsector_t bs;
    int r = load_bootsector(dev, &bs);
    if (r != FS_OK) return r;

    exfat_fs_t *fs = kmalloc(sizeof(exfat_fs_t));
    if (!fs) return FS_ERR_NOMEM;
    memset(fs, 0, sizeof(*fs));

    fs->dev = dev;
    fs->bytes_per_sector = 1u << bs.bytes_per_sector_shift;
    fs->sectors_per_cluster = 1u << bs.sectors_per_cluster_shift;
    fs->cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    fs->fat_offset_lba = bs.fat_offset;
    fs->fat_length_sectors = bs.fat_length;
    fs->number_of_fats = bs.number_of_fats ? bs.number_of_fats : 1;
    fs->cluster_heap_offset_lba = bs.cluster_heap_offset;
    fs->cluster_count = bs.cluster_count;
    fs->root_cluster = bs.first_cluster_of_root;
    fs->was_dirty = (bs.volume_flags & EXFAT_VOLUME_FLAG_DIRTY) != 0;
    fs->fat_cache_sector = UINT32_MAX;

    if (fs->bytes_per_sector < 512 || fs->bytes_per_sector != (dev->sector_size ? dev->sector_size : 512) ||
        fs->cluster_size == 0 || !cluster_valid(fs, fs->root_cluster)) {
        kfree(fs);
        LOG_ERROR("invalid boot sector fields");
        return FS_ERR_CORRUPT;
    }

    fs->fat_cache = kmalloc(fs->bytes_per_sector);
    if (!fs->fat_cache) { kfree(fs); return FS_ERR_NOMEM; }

    uint32_t c = fs->root_cluster;
    fs->root_clusters = 0;
    while (cluster_valid(fs, c) && fs->root_clusters < fs->cluster_count) {
        fs->root_clusters++;
        c = get_fat_entry(fs, c);
    }
    if (fs->root_clusters == 0) {
        exfat_free(fs);
        LOG_ERROR("root directory chain is empty");
        return FS_ERR_CORRUPT;
    }

    uint32_t upcase_cluster;
    uint64_t upcase_length;
    load_system_entries(fs, &upcase_cluster, &upcase_length);
    load_bitmap(fs);
    load_upcase(fs, upcase_cluster, upcase_length);
    fs->alloc_hint = 2;

    if (fs->was_dirty)
        LOG_WARNING("%s: volume was not cleanly unmounted, consider running a check", dev->name);

    out->type = FS_TYPE_EXFAT;
    out->dev = dev;
    out->ops = &exfat_ops;
    out->priv = fs;
    strncpy(out->label, fs->label[0] ? fs->label : "", sizeof(out->label) - 1);
    out->label[sizeof(out->label) - 1] = '\0';

    LOG_DEBUG("mounted volume '%s', cluster=%u bytes, clusters=%u, free=%u%s",
         fs->label[0] ? fs->label : "(no label)",
         (unsigned)fs->cluster_size, (unsigned)fs->cluster_count, (unsigned)fs->free_clusters,
         fs->bitmap && fs->upcase ? "" : ", read-only");

    return FS_OK;
}

static uint32_t exfat_boot_checksum(const uint8_t *region, uint32_t bytes) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < bytes; i++) {
        if (i == 106 || i == 107 || i == 112) continue;
        sum = ((sum & 1u) ? 0x80000000u : 0u) + (sum >> 1) + region[i];
    }
    return sum;
}

static uint8_t exfat_log2(uint32_t v) {
    uint8_t s = 0;
    while ((1u << s) < v) s++;
    return s;
}

static int exfat_write_chain_fat(struct block_device *dev, uint32_t bps, uint32_t fat_lba,
                                 uint32_t bitmap_clusters, uint32_t upcase_clusters) {
    uint32_t root = 2 + bitmap_clusters + upcase_clusters;
    uint32_t entries = root + 1;
    uint32_t per_sector = bps / 4;
    uint32_t *sec = kmalloc(bps);
    if (!sec) return FS_ERR_NOMEM;

    int r = FS_OK;
    for (uint32_t base = 0; base < entries && r == FS_OK; base += per_sector) {
        memset(sec, 0, bps);
        for (uint32_t i = 0; i < per_sector && base + i < entries; i++) {
            uint32_t c = base + i;
            uint32_t v;
            if (c == 0) v = 0xFFFFFFF8u;
            else if (c == 1) v = EXFAT_CLUSTER_EOC;
            else if (c < 2 + bitmap_clusters) v = (c == 1 + bitmap_clusters) ? EXFAT_CLUSTER_EOC : c + 1;
            else if (c < root) v = (c == root - 1) ? EXFAT_CLUSTER_EOC : c + 1;
            else v = EXFAT_CLUSTER_EOC;
            sec[i] = v;
        }
        if (dev->write_sectors(dev, fat_lba + base / per_sector, 1, sec) != 0) r = FS_ERR_IO;
    }
    kfree(sec);
    return r;
}

static int exfat_write_bitmap(struct block_device *dev, uint32_t bps, uint64_t lba, uint32_t used) {
    uint8_t *sec = kmalloc(bps);
    if (!sec) return FS_ERR_NOMEM;
    int r = FS_OK;
    uint32_t bits_per_sector = bps * 8u;
    for (uint32_t base = 0; base < used && r == FS_OK; base += bits_per_sector) {
        memset(sec, 0, bps);
        for (uint32_t i = 0; i < bits_per_sector && base + i < used; i++)
            sec[i >> 3] |= (uint8_t)(1u << (i & 7u));
        if (dev->write_sectors(dev, lba + base / bits_per_sector, 1, sec) != 0) r = FS_ERR_IO;
    }
    kfree(sec);
    return r;
}

int exfat_format(struct block_device *dev, const char *label, uint64_t hidden_sectors) {
    if (!dev) return FS_ERR_PARAM;
    uint32_t bps = dev->sector_size ? dev->sector_size : 512;
    if (bps < 512 || bps > 4096 || (bps & (bps - 1)) != 0) return FS_ERR_NOSUPP;

    uint64_t total = dev->sector_count;
    uint64_t bytes = total * bps;
    if (bytes < 1024ull * 1024) {
        LOG_ERROR("%s: volume is too small for exFAT", dev->name);
        return FS_ERR_NOSPACE;
    }

    uint16_t label16[11];
    int label_len = 0;
    if (label && label[0]) {
        label_len = utf8_to_utf16(label, label16, 11);
        if (label_len < 0) {
            LOG_ERROR("%s: exFAT label must be at most 11 UTF-16 characters", dev->name);
            return FS_ERR_PARAM;
        }
    }

    uint32_t cluster_bytes = bytes <= 256ull * 1024 * 1024 ? 4096u :
                             bytes <= 32ull * 1024 * 1024 * 1024 ? 32768u : 131072u;
    if (cluster_bytes < bps) cluster_bytes = bps;
    uint32_t spc = cluster_bytes / bps;

    uint64_t fat_offset = 24;
    if (spc > fat_offset) fat_offset = spc;
    uint64_t cc = (total - fat_offset) / spc;
    uint64_t fat_len = 0, heap = 0;
    for (int it = 0; it < 8; it++) {
        fat_len = ((cc + 2) * 4 + bps - 1) / bps;
        heap = (fat_offset + fat_len + spc - 1) / spc * spc;
        if (heap >= total) return FS_ERR_NOSPACE;
        uint64_t next = (total - heap) / spc;
        if (next == cc) break;
        cc = next;
    }
    fat_len = ((cc + 2) * 4 + bps - 1) / bps;
    heap = (fat_offset + fat_len + spc - 1) / spc * spc;
    if (heap >= total) return FS_ERR_NOSPACE;
    if ((total - heap) / spc < cc) cc = (total - heap) / spc;
    if (cc > 0xFFFFFFF5ull) cc = 0xFFFFFFF5ull;
    if (heap > 0xFFFFFFFFull || fat_len > 0xFFFFFFFFull) return FS_ERR_NOSUPP;

    uint64_t bitmap_bytes = (cc + 7) / 8;
    uint32_t bitmap_clusters = (uint32_t)((bitmap_bytes + cluster_bytes - 1) / cluster_bytes);
    uint32_t upcase_bytes = EXFAT_DEFAULT_UPCASE_LEN * 2u;
    uint32_t upcase_clusters = (upcase_bytes + cluster_bytes - 1) / cluster_bytes;
    uint32_t root = 2 + bitmap_clusters + upcase_clusters;
    uint32_t used = bitmap_clusters + upcase_clusters + 1;
    if (cc < (uint64_t)used + 1) {
        LOG_ERROR("%s: volume is too small for exFAT", dev->name);
        return FS_ERR_NOSPACE;
    }

    uint32_t upcase_sum = 0;
    const uint8_t *up = (const uint8_t *)exfat_default_upcase;
    for (uint32_t i = 0; i < upcase_bytes; i++)
        upcase_sum = ((upcase_sum & 1u) ? 0x80000000u : 0u) + (upcase_sum >> 1) + up[i];

    LOG_INFO("%s: creating exFAT, %llu clusters of %u bytes, FAT %llu sectors, heap at %llu",
             dev->name, (unsigned long long)cc, cluster_bytes, (unsigned long long)fat_len,
             (unsigned long long)heap);

    uint64_t bitmap_lba = heap;
    uint64_t upcase_lba = heap + (uint64_t)bitmap_clusters * spc;
    uint64_t root_lba = upcase_lba + (uint64_t)upcase_clusters * spc;

    int r = fs_dev_wipe(dev);
    if (r == FS_OK) r = fs_dev_zero(dev, 0, heap);
    if (r == FS_OK) r = exfat_write_chain_fat(dev, bps, (uint32_t)fat_offset, bitmap_clusters, upcase_clusters);
    if (r == FS_OK) r = fs_dev_zero(dev, bitmap_lba, (uint64_t)bitmap_clusters * spc);
    if (r == FS_OK) r = exfat_write_bitmap(dev, bps, bitmap_lba, used);

    uint8_t *cbuf = NULL;
    if (r == FS_OK) {
        cbuf = kmalloc((size_t)upcase_clusters * cluster_bytes);
        if (!cbuf) r = FS_ERR_NOMEM;
    }
    if (r == FS_OK) {
        memset(cbuf, 0, (size_t)upcase_clusters * cluster_bytes);
        memcpy(cbuf, exfat_default_upcase, upcase_bytes);
        if (dev->write_sectors(dev, upcase_lba, upcase_clusters * spc, cbuf) != 0)
            r = FS_ERR_IO;
    }
    if (cbuf) kfree(cbuf);

    if (r == FS_OK) r = fs_dev_zero(dev, root_lba, spc);

    uint8_t *sec = NULL;
    if (r == FS_OK) {
        sec = kmalloc((size_t)bps * 12u);
        if (!sec) r = FS_ERR_NOMEM;
    }
    if (r == FS_OK) {
        memset(sec, 0, bps);
        uint8_t *e = sec;
        if (label_len > 0) {
            e[0] = EXFAT_ENTRY_TYPE_LABEL;
            e[1] = (uint8_t)label_len;
            memcpy(e + 2, label16, (size_t)label_len * 2);
            e += 32;
        }
        uint32_t first = 2;
        e[0] = EXFAT_ENTRY_TYPE_BITMAP;
        memcpy(e + 20, &first, 4);
        memcpy(e + 24, &bitmap_bytes, 8);
        e += 32;
        uint64_t up_len = upcase_bytes;
        first = 2 + bitmap_clusters;
        e[0] = EXFAT_ENTRY_TYPE_UPCASE;
        memcpy(e + 4, &upcase_sum, 4);
        memcpy(e + 20, &first, 4);
        memcpy(e + 24, &up_len, 8);
        if (dev->write_sectors(dev, root_lba, 1, sec) != 0) r = FS_ERR_IO;
    }

    if (r == FS_OK) {
        memset(sec, 0, (size_t)bps * 12u);
        uint8_t *b = sec;
        b[0] = 0xEB; b[1] = 0x76; b[2] = 0x90;
        memcpy(b + 3, "EXFAT   ", 8);
        uint64_t q;
        uint32_t d;
        uint16_t w;
        memcpy(b + 64, &hidden_sectors, 8);
        memcpy(b + 72, &total, 8);
        d = (uint32_t)fat_offset; memcpy(b + 80, &d, 4);
        d = (uint32_t)fat_len; memcpy(b + 84, &d, 4);
        d = (uint32_t)heap; memcpy(b + 88, &d, 4);
        d = (uint32_t)cc; memcpy(b + 92, &d, 4);
        d = root; memcpy(b + 96, &d, 4);
        d = fs_new_serial(); memcpy(b + 100, &d, 4);
        w = 0x0100; memcpy(b + 104, &w, 2);
        b[108] = exfat_log2(bps);
        b[109] = exfat_log2(spc);
        b[110] = 1;
        b[111] = 0x80;
        q = (uint64_t)used * 100u / cc;
        b[112] = (uint8_t)q;
        memset(b + 120, 0xF4, 390);
        b[510] = 0x55; b[511] = 0xAA;
        for (uint32_t s = 1; s <= 8; s++) {
            uint8_t *x = sec + (size_t)s * bps;
            x[bps - 2] = 0x55;
            x[bps - 1] = 0xAA;
        }
        uint32_t sum = exfat_boot_checksum(sec, bps * 11u);
        uint32_t *cs = (uint32_t *)(sec + (size_t)bps * 11u);
        for (uint32_t i = 0; i < bps / 4; i++) cs[i] = sum;

        if (dev->write_sectors(dev, 12, 12, sec) != 0) r = FS_ERR_IO;
        if (r == FS_OK && dev->write_sectors(dev, 0, 12, sec) != 0) r = FS_ERR_IO;
    }
    if (sec) kfree(sec);
    return r;
}
