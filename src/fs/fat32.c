#include "fs/fat32.h"

#include "components/logger.h"
#include "components/Memory/heap.h"

#include <string.h>

#define FAT32_MAX_ENTRY_RUN 21

typedef struct {
    uint32_t cluster;
    uint32_t offset;
} fat32_loc_t;

static inline uint64_t cluster_to_lba(fat32_fs_t *fs, uint32_t cluster) {
    return (uint64_t)fs->data_start_lba + (uint64_t)(cluster - 2) * fs->sectors_per_cluster;
}

static inline bool is_root_pseudo(fat32_fs_t *fs, uint32_t cluster) {
    return fs->fat_bits != 32 && cluster >= FAT_ROOT_PSEUDO_BASE && cluster < FAT32_CLUSTER_EOC_MIN;
}

static int root_pseudo_span(fat32_fs_t *fs, uint32_t cluster, uint64_t *lba, uint32_t *count) {
    uint32_t first = (cluster - FAT_ROOT_PSEUDO_BASE) * fs->sectors_per_cluster;
    if (first >= fs->root_dir_sectors) return FS_ERR_PARAM;
    uint32_t n = fs->root_dir_sectors - first;
    if (n > fs->sectors_per_cluster) n = fs->sectors_per_cluster;
    *lba = (uint64_t)fs->root_dir_lba + first;
    *count = n;
    return FS_OK;
}

static int read_cluster(fat32_fs_t *fs, uint32_t cluster, void *buf) {
    if (cluster < 2) return FS_ERR_PARAM;
    if (is_root_pseudo(fs, cluster)) {
        uint64_t lba; uint32_t count;
        if (root_pseudo_span(fs, cluster, &lba, &count) != FS_OK) return FS_ERR_PARAM;
        memset(buf, 0, fs->cluster_size);
        if (fs->dev->read_sectors(fs->dev, lba, count, buf) != 0) {
            LOG_ERROR("%s: failed to read root directory sectors at %llu", fs->dev->name, (unsigned long long)lba);
            return FS_ERR_IO;
        }
        return FS_OK;
    }
    if (fs->dev->read_sectors(fs->dev, cluster_to_lba(fs, cluster), fs->sectors_per_cluster, buf) != 0) {
        LOG_ERROR("%s: failed to read cluster %u", fs->dev->name, cluster);
        return FS_ERR_IO;
    }
    return FS_OK;
}

static int write_cluster(fat32_fs_t *fs, uint32_t cluster, const void *buf) {
    if (cluster < 2) return FS_ERR_PARAM;
    if (is_root_pseudo(fs, cluster)) {
        uint64_t lba; uint32_t count;
        if (root_pseudo_span(fs, cluster, &lba, &count) != FS_OK) return FS_ERR_PARAM;
        if (fs->dev->write_sectors(fs->dev, lba, count, (void *)buf) != 0) {
            LOG_ERROR("%s: failed to write root directory sectors at %llu", fs->dev->name, (unsigned long long)lba);
            return FS_ERR_IO;
        }
        return FS_OK;
    }
    if (fs->dev->write_sectors(fs->dev, cluster_to_lba(fs, cluster), fs->sectors_per_cluster, (void *)buf) != 0) {
        LOG_ERROR("%s: failed to write cluster %u", fs->dev->name, cluster);
        return FS_ERR_IO;
    }
    return FS_OK;
}

static void fsinfo_invalidate(fat32_fs_t *fs) {
    if (fs->fsinfo_invalidated || fs->fat_bits != 32) return;
    fs->fsinfo_invalidated = true;
    if (fs->fsinfo_sector == 0 || fs->fsinfo_sector >= fs->reserved_sector_count) return;

    uint8_t *buf = kmalloc(fs->bytes_per_sector);
    if (!buf) return;
    if (fs->dev->read_sectors(fs->dev, fs->fsinfo_sector, 1, buf) == 0) {
        uint32_t lead, struc;
        memcpy(&lead, buf, 4);
        memcpy(&struc, buf + 484, 4);
        if (lead == FAT32_FSINFO_LEAD_SIG && struc == FAT32_FSINFO_STRUC_SIG) {
            uint32_t unknown = 0xFFFFFFFFu;
            memcpy(buf + 488, &unknown, 4);
            if (fs->dev->write_sectors(fs->dev, fs->fsinfo_sector, 1, buf) != 0)
                LOG_WARNING("%s: failed to update FSInfo sector", fs->dev->name);
        }
    }
    kfree(buf);
}

static int zero_cluster(fat32_fs_t *fs, uint32_t cluster) {
    uint8_t *buf = kmalloc(fs->cluster_size);
    if (!buf) return FS_ERR_NOMEM;
    memset(buf, 0, fs->cluster_size);
    int r = write_cluster(fs, cluster, buf);
    kfree(buf);
    return r;
}

static int fat_entry_location(fat32_fs_t *fs, uint32_t cluster, uint32_t *sector_in_fat, uint32_t *offset) {
    uint32_t byte_off;
    if (fs->fat_bits == 12) byte_off = cluster + cluster / 2;
    else if (fs->fat_bits == 16) byte_off = cluster * 2;
    else byte_off = cluster * 4;
    *sector_in_fat = byte_off / fs->bytes_per_sector;
    *offset = byte_off % fs->bytes_per_sector;
    return (fs->fat_bits == 12 && *offset == fs->bytes_per_sector - 1) ? 2 : 1;
}

static uint32_t get_fat_entry(fat32_fs_t *fs, uint32_t cluster) {
    if (is_root_pseudo(fs, cluster)) {
        uint32_t next_first = (cluster - FAT_ROOT_PSEUDO_BASE + 1) * fs->sectors_per_cluster;
        return next_first < fs->root_dir_sectors ? cluster + 1 : FAT32_CLUSTER_EOC;
    }

    uint32_t sector_in_fat, offset;
    int span = fat_entry_location(fs, cluster, &sector_in_fat, &offset);
    uint32_t sector = fs->fat_start_lba + sector_in_fat;

    uint8_t *buf = kmalloc((size_t)fs->bytes_per_sector * 2);
    if (!buf) return FAT32_CLUSTER_EOC;
    if (fs->dev->read_sectors(fs->dev, sector, (uint32_t)span, buf) != 0) {
        LOG_ERROR("%s: failed to read FAT sector %u", fs->dev->name, sector);
        kfree(buf);
        return FAT32_CLUSTER_EOC;
    }

    uint32_t value;
    if (fs->fat_bits == 12) {
        uint16_t raw = (uint16_t)(buf[offset] | ((uint16_t)buf[offset + 1] << 8));
        value = (cluster & 1u) ? (uint32_t)(raw >> 4) : (uint32_t)(raw & 0x0FFFu);
        if (value >= 0x0FF8u) value = FAT32_CLUSTER_EOC;
        else if (value == 0x0FF7u) value = FAT32_CLUSTER_BAD;
    } else if (fs->fat_bits == 16) {
        uint16_t raw;
        memcpy(&raw, buf + offset, 2);
        value = raw;
        if (value >= 0xFFF8u) value = FAT32_CLUSTER_EOC;
        else if (value == 0xFFF7u) value = FAT32_CLUSTER_BAD;
    } else {
        uint32_t raw;
        memcpy(&raw, buf + offset, 4);
        value = raw & FAT32_CLUSTER_MASK;
    }
    kfree(buf);
    return value;
}

static int set_fat_entry(fat32_fs_t *fs, uint32_t cluster, uint32_t value) {
    if (is_root_pseudo(fs, cluster)) return FS_OK;

    fsinfo_invalidate(fs);

    uint32_t sector_in_fat, offset;
    int span = fat_entry_location(fs, cluster, &sector_in_fat, &offset);

    uint8_t *buf = kmalloc((size_t)fs->bytes_per_sector * 2);
    if (!buf) return FS_ERR_NOMEM;

    int ret = FS_OK;
    for (uint32_t f = 0; f < fs->num_fats; f++) {
        uint32_t sector = fs->fat_start_lba + f * fs->fat_size_sectors + sector_in_fat;
        if (fs->dev->read_sectors(fs->dev, sector, (uint32_t)span, buf) != 0) { ret = FS_ERR_IO; break; }

        if (fs->fat_bits == 12) {
            uint16_t v = (value >= FAT32_CLUSTER_EOC_MIN) ? 0x0FFFu : (uint16_t)(value & 0x0FFFu);
            uint16_t raw = (uint16_t)(buf[offset] | ((uint16_t)buf[offset + 1] << 8));
            if (cluster & 1u) raw = (uint16_t)((raw & 0x000Fu) | (v << 4));
            else raw = (uint16_t)((raw & 0xF000u) | v);
            buf[offset] = (uint8_t)(raw & 0xFFu);
            buf[offset + 1] = (uint8_t)(raw >> 8);
        } else if (fs->fat_bits == 16) {
            uint16_t v = (value >= FAT32_CLUSTER_EOC_MIN) ? 0xFFFFu : (uint16_t)(value & 0xFFFFu);
            memcpy(buf + offset, &v, 2);
        } else {
            uint32_t old;
            memcpy(&old, buf + offset, 4);
            uint32_t patched = (value & FAT32_CLUSTER_MASK) | (old & ~FAT32_CLUSTER_MASK);
            memcpy(buf + offset, &patched, 4);
        }

        if (fs->dev->write_sectors(fs->dev, sector, (uint32_t)span, buf) != 0) { ret = FS_ERR_IO; break; }
    }
    kfree(buf);
    if (ret != FS_OK) LOG_ERROR("%s: failed to update FAT entry for cluster %u", fs->dev->name, cluster);
    return ret;
}

static uint32_t alloc_cluster(fat32_fs_t *fs) {
    uint32_t max_cluster = fs->total_clusters + 1;
    uint32_t cluster = (fs->next_free_hint >= 2 && fs->next_free_hint <= max_cluster) ? fs->next_free_hint : 2;

    for (uint32_t scanned = 0; scanned < fs->total_clusters; scanned++) {
        if (cluster > max_cluster) cluster = 2;

        if (get_fat_entry(fs, cluster) == FAT32_CLUSTER_FREE) {
            set_fat_entry(fs, cluster, FAT32_CLUSTER_EOC);
            fs->next_free_hint = cluster + 1;
            return cluster;
        }
        cluster++;
    }
    LOG_WARNING("%s: no free clusters left", fs->dev->name);
    return 0;
}

static void free_chain(fat32_fs_t *fs, uint32_t start) {
    uint32_t cluster = start;
    while (cluster >= 2 && cluster < FAT32_CLUSTER_EOC_MIN) {
        uint32_t next = get_fat_entry(fs, cluster);
        set_fat_entry(fs, cluster, FAT32_CLUSTER_FREE);
        cluster = next;
    }
}

static uint32_t chain_last_cluster(fat32_fs_t *fs, uint32_t start, uint32_t *out_count) {
    uint32_t cluster = start;
    uint32_t count = 0;
    uint32_t last = 0;
    while (cluster >= 2 && cluster < FAT32_CLUSTER_EOC_MIN) {
        last = cluster;
        count++;
        cluster = get_fat_entry(fs, cluster);
    }
    if (out_count) *out_count = count;
    return last;
}

static bool sfn_char_ok(char c) {
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    switch (c) {
        case '$': case '%': case '\'': case '-': case '_': case '@':
        case '~': case '`': case '!': case '(': case ')': case '{':
        case '}': case '^': case '#': case '&':
            return true;
        default:
            return false;
    }
}

static void format_83_to_name(const uint8_t *raw, char *out) {
    char base[9]; int bi = 0;
    char ext[4]; int ei = 0;
    bool lower_base = (raw[12] & 0x08) != 0;
    bool lower_ext = (raw[12] & 0x10) != 0;

    for (int i = 0; i < 8; i++) {
        uint8_t c = (i == 0 && raw[0] == FAT32_DIRENT_KANJI_E5) ? 0xE5 : raw[i];
        if (c == ' ') break;
        if (lower_base && c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
        base[bi++] = (char)c;
    }
    base[bi] = '\0';

    for (int i = 0; i < 3; i++) {
        uint8_t c = raw[8 + i];
        if (c == ' ') break;
        if (lower_ext && c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
        ext[ei++] = (char)c;
    }
    ext[ei] = '\0';

    if (ei > 0) {
        strcpy(out, base);
        strcat(out, ".");
        strcat(out, ext);
    } else {
        strcpy(out, base);
    }
}

static int fat32_name_casecmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return (int)ca - (int)cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void lfn_collect(uint16_t *lfn16, const fat32_lfn_raw_t *lfn) {
    int base = ((lfn->order & 0x1F) - 1) * 13;
    if (base < 0) return;
    uint16_t chunk[13];
    for (int k = 0; k < 5; k++) chunk[k] = lfn->name1[k];
    for (int k = 0; k < 6; k++) chunk[5 + k] = lfn->name2[k];
    for (int k = 0; k < 2; k++) chunk[11 + k] = lfn->name3[k];
    for (int k = 0; k < 13; k++) {
        if (chunk[k] == 0x0000 || chunk[k] == 0xFFFF) break;
        if (base + k < 255) lfn16[base + k] = chunk[k];
    }
}

static void lfn_to_utf8(const uint16_t *lfn16, char *out, size_t cap) {
    size_t o = 0;
    for (int i = 0; i < 255 && lfn16[i] && o + 4 < cap; i++) {
        uint32_t c = lfn16[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < 255 && lfn16[i + 1] >= 0xDC00 && lfn16[i + 1] <= 0xDFFF) {
            c = 0x10000u + ((c - 0xD800u) << 10) + (lfn16[i + 1] - 0xDC00u);
            i++;
        }
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out[o++] = (char)(0xE0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (c >> 18));
            out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = '\0';
}

static int utf8_to_lfn(const char *src, uint16_t *out, int max) {
    const uint8_t *p = (const uint8_t *)src;
    int n = 0;
    while (*p) {
        uint32_t cp;
        if (p[0] < 0x80) { cp = p[0]; p += 1; }
        else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3;
        } else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                 ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F); p += 4;
        } else return -1;
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
    return n;
}

static uint8_t lfn_checksum(const uint8_t *name11) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name11[i]);
    return sum;
}

static bool sfn_fits_exact(const char *name, uint8_t out11[11]) {
    int len = (int)strlen(name);
    if (len == 0 || len > 12) return false;

    if (strcmp(name, ".") == 0) { memset(out11, ' ', 11); out11[0] = '.'; return true; }
    if (strcmp(name, "..") == 0) { memset(out11, ' ', 11); out11[0] = '.'; out11[1] = '.'; return true; }

    int dot = -1;
    for (int i = 0; i < len; i++) {
        if (name[i] == '.') {
            if (dot != -1) return false;
            dot = i;
        }
    }

    int base_len = (dot >= 0) ? dot : len;
    int ext_len = (dot >= 0) ? (len - dot - 1) : 0;
    if (base_len == 0 || base_len > 8) return false;
    if (dot >= 0 && (ext_len == 0 || ext_len > 3)) return false;

    for (int i = 0; i < len; i++) {
        if (i == dot) continue;
        if (!sfn_char_ok(name[i])) return false;
    }

    memset(out11, ' ', 11);
    for (int i = 0; i < base_len; i++) out11[i] = name[i];
    if (dot >= 0) for (int i = 0; i < ext_len; i++) out11[8 + i] = name[dot + 1 + i];
    return true;
}

static void split_base_ext(const char *name, char *base, int *base_len, char *ext, int *ext_len) {
    int len = (int)strlen(name);
    int dot = -1;
    for (int i = len - 1; i > 0; i--) { if (name[i] == '.') { dot = i; break; } }

    int name_part_len = (dot >= 0) ? dot : len;
    int bl = 0, el = 0;

    for (int i = 0; i < name_part_len && bl < 8; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c == ' ') continue;
        if (!sfn_char_ok(c)) c = '_';
        base[bl++] = c;
    }
    if (dot >= 0) {
        for (int i = dot + 1; i < len && el < 3; i++) {
            char c = name[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            if (c == ' ') continue;
            if (!sfn_char_ok(c)) c = '_';
            ext[el++] = c;
        }
    }
    *base_len = bl; *ext_len = el;
}

static bool name_exists_83(fat32_fs_t *fs, uint32_t dir_cluster, const uint8_t name11[11]) {
    if (dir_cluster == 0) dir_cluster = fs->root_cluster;
    uint8_t *buf = kmalloc(fs->cluster_size);
    if (!buf) return true;

    uint32_t cluster = dir_cluster;
    uint32_t entries = fs->cluster_size / 32;
    bool found = false;

    while (cluster >= 2 && cluster < FAT32_CLUSTER_EOC_MIN && !found) {
        if (read_cluster(fs, cluster, buf) != FS_OK) break;
        for (uint32_t i = 0; i < entries; i++) {
            uint8_t *raw = buf + i * 32;
            if (raw[0] == FAT32_DIRENT_FREE) { cluster = 0; break; }
            if (raw[0] == FAT32_DIRENT_DELETED) continue;
            if (raw[11] == FAT32_ATTR_LFN) continue;
            if (memcmp(raw, name11, 11) == 0) { found = true; break; }
        }
        if (cluster == 0) break;
        cluster = get_fat_entry(fs, cluster);
    }

    kfree(buf);
    return found;
}

static int generate_short_name(fat32_fs_t *fs, uint32_t dir_cluster, const char *long_name,
                                uint8_t out11[11], bool *out_need_lfn) {
    if (sfn_fits_exact(long_name, out11)) {
        *out_need_lfn = false;
        return FS_OK;
    }
    *out_need_lfn = true;

    char base[9] = {0}; int bl = 0;
    char ext[4] = {0}; int el = 0;
    split_base_ext(long_name, base, &bl, ext, &el);
    if (bl == 0) { base[0] = '_'; bl = 1; }

    int basis_len = bl < 6 ? bl : 6;

    for (uint32_t n = 1; n <= 999999u; n++) {
        char num[8]; int nlen = 0;
        {
            uint32_t t = n; char tmp[8]; int tl = 0;
            while (t > 0) { tmp[tl++] = (char)('0' + (t % 10)); t /= 10; }
            for (int i = tl - 1; i >= 0; i--) num[nlen++] = tmp[i];
        }

        uint8_t cand[11];
        memset(cand, ' ', 11);
        int avail = 8 - (1 + nlen);
        if (avail > basis_len) avail = basis_len;
        if (avail < 0) avail = 0;
        memcpy(cand, base, avail);
        cand[avail] = '~';
        memcpy(cand + avail + 1, num, nlen);
        memcpy(cand + 8, ext, el);

        if (!name_exists_83(fs, dir_cluster, cand)) {
            memcpy(out11, cand, 11);
            return FS_OK;
        }
    }
    return FS_ERR_EXIST;
}

static int find_in_dir(fat32_fs_t *fs, uint32_t dir_cluster, const char *name,
                        fat32_dirent_raw_t *out_raw, fat32_loc_t *out_locs, int *out_loc_count) {
    if (dir_cluster == 0) dir_cluster = fs->root_cluster;

    uint8_t *buf = kmalloc(fs->cluster_size);
    if (!buf) return FS_ERR_NOMEM;

    uint16_t lfn16[256]; bool have_lfn = false; uint8_t lfn_checksum_expected = 0;
    fat32_loc_t pending[FAT32_MAX_ENTRY_RUN]; int pending_count = 0;

    uint32_t entries = fs->cluster_size / 32;
    uint32_t cluster = dir_cluster;
    bool done = false;

    while (cluster >= 2 && cluster < FAT32_CLUSTER_EOC_MIN && !done) {
        if (read_cluster(fs, cluster, buf) != FS_OK) { kfree(buf); return FS_ERR_IO; }

        for (uint32_t i = 0; i < entries; i++) {
            uint8_t *raw = buf + i * 32;

            if (raw[0] == FAT32_DIRENT_FREE) { done = true; break; }
            if (raw[0] == FAT32_DIRENT_DELETED) { pending_count = 0; have_lfn = false; continue; }

            uint8_t attr = raw[11];

            if (attr == FAT32_ATTR_LFN) {
                fat32_lfn_raw_t *lfn = (fat32_lfn_raw_t *)raw;
                if (pending_count < FAT32_MAX_ENTRY_RUN) {
                    pending[pending_count].cluster = cluster;
                    pending[pending_count].offset = i * 32;
                    pending_count++;
                }
                int seq = lfn->order & 0x1F;
                bool last = (lfn->order & 0x40) != 0;
                (void)seq;
                if (last) {
                    memset(lfn16, 0, sizeof(lfn16));
                    have_lfn = true;
                    lfn_checksum_expected = lfn->checksum;
                }
                lfn_collect(lfn16, lfn);
                continue;
            }

            if (pending_count < FAT32_MAX_ENTRY_RUN) {
                pending[pending_count].cluster = cluster;
                pending[pending_count].offset = i * 32;
                pending_count++;
            }

            if (attr & FAT32_ATTR_VOLUME_ID) { pending_count = 0; have_lfn = false; continue; }

            fat32_dirent_raw_t *sfn = (fat32_dirent_raw_t *)raw;
            char final_name[FS_MAX_NAME * 3 + 4];
            bool matched_via_lfn = false;

            if (have_lfn && lfn_checksum(sfn->name) == lfn_checksum_expected) {
                lfn_to_utf8(lfn16, final_name, sizeof(final_name));
                matched_via_lfn = true;
            }
            if (!matched_via_lfn) {
                format_83_to_name(sfn->name, final_name);
            }

            if (fat32_name_casecmp(final_name, name) == 0) {
                if (out_raw) *out_raw = *sfn;
                if (out_locs && out_loc_count) {
                    int cnt = pending_count > FAT32_MAX_ENTRY_RUN ? FAT32_MAX_ENTRY_RUN : pending_count;
                    for (int k = 0; k < cnt; k++) out_locs[k] = pending[k];
                    *out_loc_count = cnt;
                }
                kfree(buf);
                return FS_OK;
            }

            pending_count = 0;
            have_lfn = false;
        }

        if (done) break;
        cluster = get_fat_entry(fs, cluster);
    }

    kfree(buf);
    return FS_ERR_NOENT;
}

static int write_entries_run(fat32_fs_t *fs, uint32_t cluster, uint32_t offset,
                              const uint8_t *data, uint32_t count) {
    uint8_t *buf = kmalloc(fs->cluster_size);
    if (!buf) return FS_ERR_NOMEM;
    if (read_cluster(fs, cluster, buf) != FS_OK) { kfree(buf); return FS_ERR_IO; }
    memcpy(buf + offset, data, (size_t)count * 32);
    int r = write_cluster(fs, cluster, buf);
    kfree(buf);
    return r;
}

static int find_free_slot_run(fat32_fs_t *fs, uint32_t dir_cluster, uint32_t need,
                               uint32_t *out_cluster, uint32_t *out_offset) {
    if (dir_cluster == 0) dir_cluster = fs->root_cluster;

    uint32_t entries_per_cluster = fs->cluster_size / 32;
    if (need > entries_per_cluster) return FS_ERR_NOSUPP;

    uint8_t *buf = kmalloc(fs->cluster_size);
    if (!buf) return FS_ERR_NOMEM;

    uint32_t cluster = dir_cluster;
    uint32_t last_cluster = 0;

    while (cluster >= 2 && cluster < FAT32_CLUSTER_EOC_MIN) {
        if (read_cluster(fs, cluster, buf) != FS_OK) { kfree(buf); return FS_ERR_IO; }

        uint32_t run_start = 0, run_len = 0;
        bool end_reached = false;

        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            uint8_t b0 = buf[i * 32];
            bool free_slot = end_reached || b0 == FAT32_DIRENT_FREE || b0 == FAT32_DIRENT_DELETED;
            if (b0 == FAT32_DIRENT_FREE) end_reached = true;

            if (free_slot) {
                if (run_len == 0) run_start = i;
                run_len++;
                if (run_len >= need) {
                    kfree(buf);
                    *out_cluster = cluster;
                    *out_offset = run_start * 32;
                    return FS_OK;
                }
            } else {
                run_len = 0;
            }
        }

        if (end_reached) {
            for (uint32_t i = 0; i < entries_per_cluster; i++)
                if (buf[i * 32] == FAT32_DIRENT_FREE) buf[i * 32] = FAT32_DIRENT_DELETED;
            if (write_cluster(fs, cluster, buf) != FS_OK) { kfree(buf); return FS_ERR_IO; }
        }

        last_cluster = cluster;
        cluster = get_fat_entry(fs, cluster);
    }

    if (last_cluster && is_root_pseudo(fs, last_cluster)) { kfree(buf); return FS_ERR_NOSPACE; }

    uint32_t newc = alloc_cluster(fs);
    if (!newc) { kfree(buf); return FS_ERR_NOSPACE; }
    if (zero_cluster(fs, newc) != FS_OK) { kfree(buf); return FS_ERR_IO; }

    if (last_cluster) {
        if (set_fat_entry(fs, last_cluster, newc) != FS_OK) { kfree(buf); return FS_ERR_IO; }
    }
    set_fat_entry(fs, newc, FAT32_CLUSTER_EOC);

    kfree(buf);
    *out_cluster = newc;
    *out_offset = 0;
    return FS_OK;
}

static int create_dirent(fat32_fs_t *fs, uint32_t parent_cluster, const char *name,
                          uint8_t attr, uint32_t first_cluster, uint32_t size,
                          uint32_t *out_dirent_cluster, uint32_t *out_dirent_offset) {
    fat32_dirent_raw_t existing;
    fat32_loc_t locs[FAT32_MAX_ENTRY_RUN]; int loc_count = 0;
    if (find_in_dir(fs, parent_cluster, name, &existing, locs, &loc_count) == FS_OK)
        return FS_ERR_EXIST;

    uint8_t sfn11[11]; bool need_lfn;
    int r = generate_short_name(fs, parent_cluster, name, sfn11, &need_lfn);
    if (r != FS_OK) return r;

    uint16_t name16[256];
    int name_len = utf8_to_lfn(name, name16, 255);
    if (name_len <= 0) return FS_ERR_PARAM;
    int lfn_count = need_lfn ? (name_len + 12) / 13 : 0;
    if (lfn_count > 20) return FS_ERR_PARAM;

    uint32_t need_entries = (uint32_t)(lfn_count + 1);
    uint32_t first_cl, first_off;
    r = find_free_slot_run(fs, parent_cluster, need_entries, &first_cl, &first_off);
    if (r != FS_OK) return r;

    uint8_t entry_buf[FAT32_MAX_ENTRY_RUN * 32];
    memset(entry_buf, 0, sizeof(entry_buf));

    if (need_lfn) {
        uint8_t chk = lfn_checksum(sfn11);
        for (int seq = lfn_count; seq >= 1; seq--) {
            fat32_lfn_raw_t *e = (fat32_lfn_raw_t *)(entry_buf + (lfn_count - seq) * 32);
            e->order = (uint8_t)(seq | (seq == lfn_count ? 0x40 : 0));
            e->attr = FAT32_ATTR_LFN;
            e->type = 0;
            e->checksum = chk;
            e->first_cluster_lo = 0;

            int base = (seq - 1) * 13;
            uint16_t chunk[13];
            for (int k = 0; k < 13; k++) {
                int idx = base + k;
                if (idx < name_len) chunk[k] = name16[idx];
                else if (idx == name_len) chunk[k] = 0x0000;
                else chunk[k] = 0xFFFF;
            }
            for (int k = 0; k < 5; k++) e->name1[k] = chunk[k];
            for (int k = 0; k < 6; k++) e->name2[k] = chunk[5 + k];
            for (int k = 0; k < 2; k++) e->name3[k] = chunk[11 + k];
        }
    }

    fat32_dirent_raw_t *sfn = (fat32_dirent_raw_t *)(entry_buf + lfn_count * 32);
    memcpy(sfn->name, sfn11, 11);
    sfn->attr = attr;
    sfn->first_cluster_hi = (uint16_t)(first_cluster >> 16);
    sfn->first_cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
    sfn->file_size = size;
    sfn->create_date = sfn->write_date = sfn->last_access_date = 0x21;

    r = write_entries_run(fs, first_cl, first_off, entry_buf, need_entries);
    if (r != FS_OK) return r;

    if (out_dirent_cluster) *out_dirent_cluster = first_cl;
    if (out_dirent_offset) *out_dirent_offset = first_off + (uint32_t)lfn_count * 32;
    return FS_OK;
}

static int mark_entries_deleted(fat32_fs_t *fs, const fat32_loc_t *locs, int count) {
    uint8_t *buf = kmalloc(fs->cluster_size);
    if (!buf) return FS_ERR_NOMEM;
    int ret = FS_OK;
    for (int i = 0; i < count; i++) {
        if (read_cluster(fs, locs[i].cluster, buf) != FS_OK) { ret = FS_ERR_IO; break; }
        buf[locs[i].offset] = FAT32_DIRENT_DELETED;
        if (write_cluster(fs, locs[i].cluster, buf) != FS_OK) { ret = FS_ERR_IO; break; }
    }
    kfree(buf);
    return ret;
}

static int patch_dirent(fat32_fs_t *fs, uint32_t cluster, uint32_t offset,
                         uint32_t first_cluster, uint32_t size) {
    uint8_t *buf = kmalloc(fs->cluster_size);
    if (!buf) return FS_ERR_NOMEM;
    if (read_cluster(fs, cluster, buf) != FS_OK) { kfree(buf); return FS_ERR_IO; }
    fat32_dirent_raw_t *e = (fat32_dirent_raw_t *)(buf + offset);
    e->first_cluster_hi = (uint16_t)(first_cluster >> 16);
    e->first_cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
    e->file_size = size;
    int r = write_cluster(fs, cluster, buf);
    kfree(buf);
    return r;
}

static uint32_t descend_cluster(fat32_fs_t *fs, uint32_t entry_first_cluster) {
    return entry_first_cluster == 0 ? fs->root_cluster : entry_first_cluster;
}

static int resolve_dir_path(fat32_fs_t *fs, const char *path, uint32_t *out_cluster) {
    fs_path_parts_t pp;
    int r = fs_split_path(path, &pp);
    if (r != FS_OK) return r;

    uint32_t cluster = fs->root_cluster;
    for (int i = 0; i < pp.count; i++) {
        fat32_dirent_raw_t e;
        r = find_in_dir(fs, cluster, pp.comps[i], &e, NULL, NULL);
        if (r != FS_OK) return r;
        if (!(e.attr & FAT32_ATTR_DIRECTORY)) return FS_ERR_NOTDIR;
        cluster = descend_cluster(fs, ((uint32_t)e.first_cluster_hi << 16) | e.first_cluster_lo);
    }
    *out_cluster = cluster;
    return FS_OK;
}

static int resolve_parent(fat32_fs_t *fs, const char *path, char *leaf_out, size_t leaf_out_sz,
                           uint32_t *parent_cluster_out) {
    fs_path_parts_t pp;
    int r = fs_split_path(path, &pp);
    if (r != FS_OK) return r;
    if (pp.count == 0) return FS_ERR_PARAM;

    uint32_t cluster = fs->root_cluster;
    for (int i = 0; i < pp.count - 1; i++) {
        fat32_dirent_raw_t e;
        r = find_in_dir(fs, cluster, pp.comps[i], &e, NULL, NULL);
        if (r != FS_OK) return r;
        if (!(e.attr & FAT32_ATTR_DIRECTORY)) return FS_ERR_NOTDIR;
        cluster = descend_cluster(fs, ((uint32_t)e.first_cluster_hi << 16) | e.first_cluster_lo);
    }

    const char *leaf = pp.comps[pp.count - 1];
    if (strlen(leaf) >= leaf_out_sz) return FS_ERR_PARAM;
    strcpy(leaf_out, leaf);
    *parent_cluster_out = cluster;
    return FS_OK;
}

static uint32_t file_cluster_at(fat32_fs_t *fs, fat32_file_t *ff, uint32_t index) {
    if (ff->first_cluster < 2) return 0;

    if (index >= ff->cache_index && ff->cache_cluster >= 2) {
        uint32_t cluster = ff->cache_cluster;
        for (uint32_t i = ff->cache_index; i < index; i++) {
            cluster = get_fat_entry(fs, cluster);
            if (cluster < 2 || cluster >= FAT32_CLUSTER_EOC_MIN) return 0;
        }
        ff->cache_index = index;
        ff->cache_cluster = cluster;
        return cluster;
    }

    uint32_t cluster = ff->first_cluster;
    for (uint32_t i = 0; i < index; i++) {
        cluster = get_fat_entry(fs, cluster);
        if (cluster < 2 || cluster >= FAT32_CLUSTER_EOC_MIN) return 0;
    }
    ff->cache_index = index;
    ff->cache_cluster = cluster;
    return cluster;
}

static int file_ensure_capacity(fat32_fs_t *fs, fat32_file_t *ff, uint64_t needed_bytes) {
    uint32_t needed_clusters = (uint32_t)((needed_bytes + fs->cluster_size - 1) / fs->cluster_size);
    if (needed_clusters == 0) needed_clusters = 1;

    while (ff->alloc_clusters < needed_clusters) {
        uint32_t newc = alloc_cluster(fs);
        if (!newc) return FS_ERR_NOSPACE;
        if (zero_cluster(fs, newc) != FS_OK) return FS_ERR_IO;

        if (ff->first_cluster < 2) {
            ff->first_cluster = newc;
            ff->last_cluster = newc;
            ff->cache_index = 0;
            ff->cache_cluster = newc;
        } else {
            if (set_fat_entry(fs, ff->last_cluster, newc) != FS_OK) return FS_ERR_IO;
            ff->last_cluster = newc;
        }
        ff->alloc_clusters++;
        ff->dirty = true;
    }
    return FS_OK;
}

static int file_zero_range(fat32_fs_t *fs, fat32_file_t *ff, uint64_t from, uint64_t to) {
    while (from < to) {
        uint32_t cluster_index = (uint32_t)(from / fs->cluster_size);
        uint32_t off = (uint32_t)(from % fs->cluster_size);
        uint32_t cluster = file_cluster_at(fs, ff, cluster_index);
        if (cluster < 2) return FS_ERR_IO;

        uint64_t chunk = fs->cluster_size - off;
        if (chunk > to - from) chunk = to - from;

        if (read_cluster(fs, cluster, ff->iobuf) != FS_OK) return FS_ERR_IO;
        memset(ff->iobuf + off, 0, (size_t)chunk);
        if (write_cluster(fs, cluster, ff->iobuf) != FS_OK) return FS_ERR_IO;
        from += chunk;
    }
    return FS_OK;
}

static int file_grow_to(fat32_fs_t *fs, fat32_file_t *ff, uint64_t old_size, uint64_t new_size) {
    uint64_t old_alloc_bytes = (uint64_t)ff->alloc_clusters * fs->cluster_size;
    if (file_ensure_capacity(fs, ff, new_size) != FS_OK) return FS_ERR_NOSPACE;
    uint64_t stale_end = new_size < old_alloc_bytes ? new_size : old_alloc_bytes;
    if (old_size < stale_end) return file_zero_range(fs, ff, old_size, stale_end);
    return FS_OK;
}

static int fat32_op_open(fs_t *fsroot, const char *path, bool create, bool truncate_flag, fs_file_t *out) {
    fat32_fs_t *fs = (fat32_fs_t *)fsroot->priv;

    char leaf[FS_MAX_NAME + 1];
    uint32_t parent_cluster;
    int r = resolve_parent(fs, path, leaf, sizeof(leaf), &parent_cluster);
    if (r != FS_OK) return r;

    fat32_dirent_raw_t e;
    fat32_loc_t locs[FAT32_MAX_ENTRY_RUN]; int loc_count = 0;
    uint32_t dirent_cluster, dirent_offset;
    uint32_t first_cluster, file_size;

    r = find_in_dir(fs, parent_cluster, leaf, &e, locs, &loc_count);
    if (r == FS_OK) {
        if (e.attr & FAT32_ATTR_DIRECTORY) return FS_ERR_ISDIR;
        dirent_cluster = locs[loc_count - 1].cluster;
        dirent_offset = locs[loc_count - 1].offset;
        first_cluster = ((uint32_t)e.first_cluster_hi << 16) | e.first_cluster_lo;
        file_size = e.file_size;

        if (truncate_flag && (first_cluster || file_size)) {
            if (first_cluster >= 2) free_chain(fs, first_cluster);
            first_cluster = 0;
            file_size = 0;
            patch_dirent(fs, dirent_cluster, dirent_offset, 0, 0);
        }
    } else if (r == FS_ERR_NOENT && create) {
        r = create_dirent(fs, parent_cluster, leaf, FAT32_ATTR_ARCHIVE, 0, 0, &dirent_cluster, &dirent_offset);
        if (r != FS_OK) return r;
        first_cluster = 0;
        file_size = 0;
    } else {
        return r;
    }

    fat32_file_t *ff = kmalloc(sizeof(fat32_file_t));
    if (!ff) return FS_ERR_NOMEM;
    memset(ff, 0, sizeof(*ff));
    ff->fs = fs;
    ff->first_cluster = first_cluster;
    ff->dirent_cluster = dirent_cluster;
    ff->dirent_offset = dirent_offset;
    ff->cache_index = 0;
    ff->cache_cluster = first_cluster;

    if (first_cluster >= 2) {
        uint32_t count = 0;
        ff->last_cluster = chain_last_cluster(fs, first_cluster, &count);
        ff->alloc_clusters = count;
    } else {
        ff->last_cluster = 0;
        ff->alloc_clusters = 0;
    }

    ff->iobuf = kmalloc(fs->cluster_size);
    if (!ff->iobuf) { kfree(ff); return FS_ERR_NOMEM; }

    out->priv = ff;
    out->size = file_size;
    out->pos = 0;
    out->writable = true;
    return FS_OK;
}

static int fat32_op_close(fs_file_t *f) {
    fat32_file_t *ff = (fat32_file_t *)f->priv;
    if (!ff) return FS_OK;

    if (ff->dirty) {
        patch_dirent(ff->fs, ff->dirent_cluster, ff->dirent_offset, ff->first_cluster, (uint32_t)f->size);
    }
    if (ff->iobuf) kfree(ff->iobuf);
    kfree(ff);
    f->priv = NULL;
    return FS_OK;
}

static int64_t fat32_op_read(fs_file_t *f, void *buf, uint64_t size) {
    fat32_file_t *ff = (fat32_file_t *)f->priv;
    fat32_fs_t *fs = ff->fs;

    if (f->pos >= f->size) return 0;
    uint64_t remaining = f->size - f->pos;
    if (size > remaining) size = remaining;

    uint8_t *out = (uint8_t *)buf;
    uint64_t done = 0;

    while (done < size) {
        uint32_t cluster_index = (uint32_t)(f->pos / fs->cluster_size);
        uint32_t off_in_cluster = (uint32_t)(f->pos % fs->cluster_size);
        uint32_t cluster = file_cluster_at(fs, ff, cluster_index);
        if (cluster < 2) break;

        if (read_cluster(fs, cluster, ff->iobuf) != FS_OK) break;

        uint64_t chunk = fs->cluster_size - off_in_cluster;
        if (chunk > size - done) chunk = size - done;

        memcpy(out + done, ff->iobuf + off_in_cluster, (size_t)chunk);
        done += chunk;
        f->pos += chunk;
    }

    return (int64_t)done;
}

static int64_t fat32_op_write(fs_file_t *f, const void *buf, uint64_t size) {
    fat32_file_t *ff = (fat32_file_t *)f->priv;
    fat32_fs_t *fs = ff->fs;
    if (size == 0) return 0;

    if (f->pos > f->size) {
        if (file_grow_to(fs, ff, f->size, f->pos) != FS_OK) return FS_ERR_NOSPACE;
        f->size = f->pos;
        ff->dirty = true;
    }
    if (file_ensure_capacity(fs, ff, f->pos + size) != FS_OK) return FS_ERR_NOSPACE;

    const uint8_t *in = (const uint8_t *)buf;
    uint64_t done = 0;

    while (done < size) {
        uint32_t cluster_index = (uint32_t)(f->pos / fs->cluster_size);
        uint32_t off_in_cluster = (uint32_t)(f->pos % fs->cluster_size);
        uint32_t cluster = file_cluster_at(fs, ff, cluster_index);
        if (cluster < 2) break;

        uint64_t chunk = fs->cluster_size - off_in_cluster;
        if (chunk > size - done) chunk = size - done;

        if (read_cluster(fs, cluster, ff->iobuf) != FS_OK) break;
        memcpy(ff->iobuf + off_in_cluster, in + done, (size_t)chunk);
        if (write_cluster(fs, cluster, ff->iobuf) != FS_OK) break;

        done += chunk;
        f->pos += chunk;
    }

    if (f->pos > f->size) { f->size = f->pos; ff->dirty = true; }
    if (done > 0) ff->dirty = true;

    return (int64_t)done;
}

static int fat32_op_seek(fs_file_t *f, uint64_t pos) {
    f->pos = pos;
    return FS_OK;
}

static int fat32_op_truncate(fs_file_t *f, uint64_t size) {
    fat32_file_t *ff = (fat32_file_t *)f->priv;
    fat32_fs_t *fs = ff->fs;

    if (size < f->size) {
        uint32_t needed_clusters = (uint32_t)((size + fs->cluster_size - 1) / fs->cluster_size);
        if (needed_clusters == 0) {
            if (ff->first_cluster >= 2) free_chain(fs, ff->first_cluster);
            ff->first_cluster = 0;
            ff->last_cluster = 0;
            ff->alloc_clusters = 0;
            ff->cache_index = 0;
            ff->cache_cluster = 0;
        } else if (needed_clusters < ff->alloc_clusters) {
            uint32_t keep_last = file_cluster_at(fs, ff, needed_clusters - 1);
            uint32_t next = get_fat_entry(fs, keep_last);
            set_fat_entry(fs, keep_last, FAT32_CLUSTER_EOC);
            if (next >= 2 && next < FAT32_CLUSTER_EOC_MIN) free_chain(fs, next);
            ff->last_cluster = keep_last;
            ff->alloc_clusters = needed_clusters;
        }
        f->size = size;
        if (f->pos > size) f->pos = size;
        ff->dirty = true;
    } else if (size > f->size) {
        if (file_grow_to(fs, ff, f->size, size) != FS_OK) return FS_ERR_NOSPACE;
        f->size = size;
        ff->dirty = true;
    }

    patch_dirent(fs, ff->dirent_cluster, ff->dirent_offset, ff->first_cluster, (uint32_t)f->size);
    ff->dirty = false;
    return FS_OK;
}

static int fat32_op_opendir(fs_t *fsroot, const char *path, fs_dir_t *out) {
    fat32_fs_t *fs = (fat32_fs_t *)fsroot->priv;
    uint32_t cluster;
    int r = resolve_dir_path(fs, path, &cluster);
    if (r != FS_OK) return r;

    fat32_diriter_t *it = kmalloc(sizeof(fat32_diriter_t));
    if (!it) return FS_ERR_NOMEM;
    it->fs = fs;
    it->dir_start_cluster = cluster;
    it->cur_cluster = cluster;
    it->entry_index = 0;
    it->cluster_buf = kmalloc(fs->cluster_size);
    if (!it->cluster_buf) { kfree(it); return FS_ERR_NOMEM; }
    if (read_cluster(fs, cluster, it->cluster_buf) != FS_OK) {
        kfree(it->cluster_buf); kfree(it); return FS_ERR_IO;
    }

    out->priv = it;
    return FS_OK;
}

static int fat32_op_readdir(fs_dir_t *dir, fs_dirent_t *out) {
    fat32_diriter_t *it = (fat32_diriter_t *)dir->priv;
    fat32_fs_t *fs = it->fs;
    uint32_t entries = fs->cluster_size / 32;

    uint16_t lfn16[256]; bool have_lfn = false; uint8_t lfn_checksum_expected = 0;
    lfn16[0] = 0;

    while (it->cur_cluster >= 2 && it->cur_cluster < FAT32_CLUSTER_EOC_MIN) {
        while (it->entry_index < entries) {
            uint8_t *raw = it->cluster_buf + it->entry_index * 32;
            it->entry_index++;

            if (raw[0] == FAT32_DIRENT_FREE) return FS_ERR_EOF;
            if (raw[0] == FAT32_DIRENT_DELETED) { have_lfn = false; continue; }

            uint8_t attr = raw[11];
            if (attr == FAT32_ATTR_LFN) {
                fat32_lfn_raw_t *lfn = (fat32_lfn_raw_t *)raw;
                bool last = (lfn->order & 0x40) != 0;
                if (last) { memset(lfn16, 0, sizeof(lfn16)); have_lfn = true; lfn_checksum_expected = lfn->checksum; }
                lfn_collect(lfn16, lfn);
                continue;
            }

            if (attr & FAT32_ATTR_VOLUME_ID) { have_lfn = false; continue; }

            fat32_dirent_raw_t *sfn = (fat32_dirent_raw_t *)raw;
            if (have_lfn && lfn_checksum(sfn->name) == lfn_checksum_expected) {
                lfn_to_utf8(lfn16, out->name, sizeof(out->name));
            } else {
                format_83_to_name(sfn->name, out->name);
            }
            out->type = (attr & FAT32_ATTR_DIRECTORY) ? FS_ENTRY_DIR : FS_ENTRY_FILE;
            out->size = sfn->file_size;
            return FS_OK;
        }

        uint32_t next = get_fat_entry(fs, it->cur_cluster);
        if (next < 2 || next >= FAT32_CLUSTER_EOC_MIN) return FS_ERR_EOF;
        it->cur_cluster = next;
        it->entry_index = 0;
        if (read_cluster(fs, it->cur_cluster, it->cluster_buf) != FS_OK) return FS_ERR_IO;
    }
    return FS_ERR_EOF;
}

static int fat32_op_closedir(fs_dir_t *dir) {
    fat32_diriter_t *it = (fat32_diriter_t *)dir->priv;
    if (it) {
        if (it->cluster_buf) kfree(it->cluster_buf);
        kfree(it);
    }
    dir->priv = NULL;
    return FS_OK;
}

static int fat32_op_mkdir(fs_t *fsroot, const char *path) {
    fat32_fs_t *fs = (fat32_fs_t *)fsroot->priv;

    char leaf[FS_MAX_NAME + 1];
    uint32_t parent_cluster;
    int r = resolve_parent(fs, path, leaf, sizeof(leaf), &parent_cluster);
    if (r != FS_OK) return r;

    fat32_dirent_raw_t existing;
    if (find_in_dir(fs, parent_cluster, leaf, &existing, NULL, NULL) == FS_OK)
        return FS_ERR_EXIST;

    uint32_t newc = alloc_cluster(fs);
    if (!newc) return FS_ERR_NOSPACE;
    if (zero_cluster(fs, newc) != FS_OK) return FS_ERR_IO;

    uint8_t *buf = kmalloc(fs->cluster_size);
    if (!buf) { free_chain(fs, newc); return FS_ERR_NOMEM; }
    memset(buf, 0, fs->cluster_size);

    fat32_dirent_raw_t *dot = (fat32_dirent_raw_t *)buf;
    memset(dot->name, ' ', 11); dot->name[0] = '.';
    dot->attr = FAT32_ATTR_DIRECTORY;
    dot->create_date = dot->write_date = dot->last_access_date = 0x21;
    dot->first_cluster_hi = (uint16_t)(newc >> 16);
    dot->first_cluster_lo = (uint16_t)(newc & 0xFFFF);

    fat32_dirent_raw_t *dotdot = (fat32_dirent_raw_t *)(buf + 32);
    memset(dotdot->name, ' ', 11); dotdot->name[0] = '.'; dotdot->name[1] = '.';
    dotdot->attr = FAT32_ATTR_DIRECTORY;
    dotdot->create_date = dotdot->write_date = dotdot->last_access_date = 0x21;
    uint32_t dotdot_cluster = (parent_cluster == fs->root_cluster) ? 0 : parent_cluster;
    dotdot->first_cluster_hi = (uint16_t)(dotdot_cluster >> 16);
    dotdot->first_cluster_lo = (uint16_t)(dotdot_cluster & 0xFFFF);

    r = write_cluster(fs, newc, buf);
    kfree(buf);
    if (r != FS_OK) { free_chain(fs, newc); return r; }

    uint32_t dc, doff;
    r = create_dirent(fs, parent_cluster, leaf, FAT32_ATTR_DIRECTORY, newc, 0, &dc, &doff);
    if (r != FS_OK) { free_chain(fs, newc); return r; }

    return FS_OK;
}

static int fat32_op_stat(fs_t *fsroot, const char *path, fs_dirent_t *out) {
    fat32_fs_t *fs = (fat32_fs_t *)fsroot->priv;

    fs_path_parts_t pp;
    int r = fs_split_path(path, &pp);
    if (r != FS_OK) return r;

    if (pp.count == 0) {
        strncpy(out->name, "/", FS_MAX_NAME); out->name[FS_MAX_NAME] = '\0';
        out->type = FS_ENTRY_DIR;
        out->size = 0;
        return FS_OK;
    }

    char leaf[FS_MAX_NAME + 1];
    uint32_t parent_cluster;
    r = resolve_parent(fs, path, leaf, sizeof(leaf), &parent_cluster);
    if (r != FS_OK) return r;

    fat32_dirent_raw_t e;
    r = find_in_dir(fs, parent_cluster, leaf, &e, NULL, NULL);
    if (r != FS_OK) return r;

    strncpy(out->name, leaf, FS_MAX_NAME); out->name[FS_MAX_NAME] = '\0';
    out->type = (e.attr & FAT32_ATTR_DIRECTORY) ? FS_ENTRY_DIR : FS_ENTRY_FILE;
    out->size = e.file_size;
    return FS_OK;
}

static int fat32_op_unlink(fs_t *fsroot, const char *path) {
    fat32_fs_t *fs = (fat32_fs_t *)fsroot->priv;

    char leaf[FS_MAX_NAME + 1];
    uint32_t parent_cluster;
    int r = resolve_parent(fs, path, leaf, sizeof(leaf), &parent_cluster);
    if (r != FS_OK) return r;

    fat32_dirent_raw_t e;
    fat32_loc_t locs[FAT32_MAX_ENTRY_RUN]; int loc_count = 0;
    r = find_in_dir(fs, parent_cluster, leaf, &e, locs, &loc_count);
    if (r != FS_OK) return r;
    if (e.attr & FAT32_ATTR_DIRECTORY) return FS_ERR_ISDIR;

    uint32_t first_cluster = ((uint32_t)e.first_cluster_hi << 16) | e.first_cluster_lo;
    if (first_cluster >= 2) free_chain(fs, first_cluster);

    return mark_entries_deleted(fs, locs, loc_count);
}

static int fat32_op_rmdir(fs_t *fsroot, const char *path) {
    fat32_fs_t *fs = (fat32_fs_t *)fsroot->priv;

    char leaf[FS_MAX_NAME + 1];
    uint32_t parent_cluster;
    int r = resolve_parent(fs, path, leaf, sizeof(leaf), &parent_cluster);
    if (r != FS_OK) return r;

    fat32_dirent_raw_t e;
    fat32_loc_t locs[FAT32_MAX_ENTRY_RUN]; int loc_count = 0;
    r = find_in_dir(fs, parent_cluster, leaf, &e, locs, &loc_count);
    if (r != FS_OK) return r;
    if (!(e.attr & FAT32_ATTR_DIRECTORY)) return FS_ERR_NOTDIR;

    uint32_t dir_cluster = descend_cluster(fs, ((uint32_t)e.first_cluster_hi << 16) | e.first_cluster_lo);

    fs_dir_t it;
    fat32_diriter_t iter;
    iter.fs = fs; iter.dir_start_cluster = dir_cluster; iter.cur_cluster = dir_cluster; iter.entry_index = 0;
    iter.cluster_buf = kmalloc(fs->cluster_size);
    if (!iter.cluster_buf) return FS_ERR_NOMEM;
    if (read_cluster(fs, dir_cluster, iter.cluster_buf) != FS_OK) { kfree(iter.cluster_buf); return FS_ERR_IO; }

    it.fs = NULL; it.priv = &iter;
    fs_dirent_t entv;
    bool empty = true;
    while (fat32_op_readdir(&it, &entv) == FS_OK) {
        if (strcmp(entv.name, ".") == 0 || strcmp(entv.name, "..") == 0) continue;
        empty = false;
        break;
    }
    kfree(iter.cluster_buf);

    if (!empty) return FS_ERR_NOTEMPTY;

    if (dir_cluster >= 2) free_chain(fs, dir_cluster);
    return mark_entries_deleted(fs, locs, loc_count);
}

static int fat32_op_unmount(fs_t *fsroot) {
    if (fsroot->priv) kfree(fsroot->priv);
    fsroot->priv = NULL;
    return FS_OK;
}

const fs_ops_t fat32_ops = {
    .open = fat32_op_open,
    .close = fat32_op_close,
    .read = fat32_op_read,
    .write = fat32_op_write,
    .seek = fat32_op_seek,
    .truncate = fat32_op_truncate,
    .opendir = fat32_op_opendir,
    .readdir = fat32_op_readdir,
    .closedir = fat32_op_closedir,
    .mkdir = fat32_op_mkdir,
    .unlink = fat32_op_unlink,
    .rmdir = fat32_op_rmdir,
    .stat = fat32_op_stat,
    .unmount = fat32_op_unmount,
};

typedef struct {
    uint8_t fat_bits;
    uint32_t fat_size;
    uint32_t total_sectors;
    uint32_t root_dir_sectors;
    uint32_t data_start;
    uint32_t total_clusters;
} fat_geometry_t;

static int read_and_validate_bpb(struct block_device *dev, uint8_t *sector_buf, fat32_bpb_t **out_bpb,
                                 fat_geometry_t *out_geo) {
    if (dev->read_sectors(dev, 0, 1, sector_buf) != 0) {
        LOG_ERROR("%s: failed to read boot sector", dev->name);
        return FS_ERR_IO;
    }

    if (sector_buf[FAT32_BOOT_SIG_OFFSET] != FAT32_BOOT_SIG_0 ||
        sector_buf[FAT32_BOOT_SIG_OFFSET + 1] != FAT32_BOOT_SIG_1)
        return FS_ERR_CORRUPT;

    fat32_bpb_t *bpb = (fat32_bpb_t *)sector_buf;
    uint16_t bps = bpb->bytes_per_sector;
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) return FS_ERR_CORRUPT;
    uint8_t spc = bpb->sectors_per_cluster;
    if (spc == 0 || (spc & (spc - 1)) != 0) return FS_ERR_CORRUPT;
    if (bpb->num_fats == 0 || bpb->reserved_sector_count == 0) return FS_ERR_CORRUPT;
    if (bps != (dev->sector_size ? dev->sector_size : 512)) return FS_ERR_CORRUPT;

    fat_geometry_t geo;
    geo.fat_size = bpb->fat_size_16 ? bpb->fat_size_16 : bpb->fat_size_32;
    if (geo.fat_size == 0) return FS_ERR_CORRUPT;
    geo.total_sectors = bpb->total_sectors_16 ? bpb->total_sectors_16 : bpb->total_sectors_32;
    geo.root_dir_sectors = ((uint32_t)bpb->root_entry_count * 32u + bps - 1u) / bps;
    geo.data_start = bpb->reserved_sector_count + (uint32_t)bpb->num_fats * geo.fat_size + geo.root_dir_sectors;
    if (geo.total_sectors <= geo.data_start) return FS_ERR_CORRUPT;
    geo.total_clusters = (geo.total_sectors - geo.data_start) / spc;

    if (geo.total_clusters < 4085) geo.fat_bits = 12;
    else if (geo.total_clusters < 65525) geo.fat_bits = 16;
    else geo.fat_bits = 32;

    if (geo.fat_bits == 32) {
        if (bpb->root_entry_count != 0 || bpb->fat_size_16 != 0 || bpb->fat_size_32 == 0) return FS_ERR_CORRUPT;
        if (bpb->root_cluster < 2) return FS_ERR_CORRUPT;
    } else {
        if (bpb->root_entry_count == 0 || bpb->fat_size_16 == 0) return FS_ERR_CORRUPT;
        uint64_t fat_bytes = (uint64_t)geo.fat_size * bps;
        uint64_t need = (geo.fat_bits == 12) ? ((uint64_t)(geo.total_clusters + 2) * 3 + 1) / 2
                                             : (uint64_t)(geo.total_clusters + 2) * 2;
        if (fat_bytes < need) return FS_ERR_CORRUPT;
    }

    if (out_bpb) *out_bpb = bpb;
    if (out_geo) *out_geo = geo;
    return FS_OK;
}

int fat32_probe(struct block_device *dev) {
    if (!dev) return FS_ERR_PARAM;
    uint32_t sector_size = dev->sector_size ? dev->sector_size : 512;
    uint8_t *buf = kmalloc(sector_size);
    if (!buf) return FS_ERR_NOMEM;
    int r = read_and_validate_bpb(dev, buf, NULL, NULL);
    kfree(buf);
    return r;
}

int fat32_mount(struct block_device *dev, fs_t *out) {
    if (!dev || !out) return FS_ERR_PARAM;

    uint32_t sector_size = dev->sector_size ? dev->sector_size : 512;
    uint8_t *buf = kmalloc(sector_size);
    if (!buf) return FS_ERR_NOMEM;

    fat32_bpb_t *bpb;
    fat_geometry_t geo;
    int r = read_and_validate_bpb(dev, buf, &bpb, &geo);
    if (r != FS_OK) { kfree(buf); return r; }

    fat32_fs_t *fs = kmalloc(sizeof(fat32_fs_t));
    if (!fs) { kfree(buf); return FS_ERR_NOMEM; }
    memset(fs, 0, sizeof(*fs));

    fs->dev = dev;
    fs->fat_bits = geo.fat_bits;
    fs->bytes_per_sector = bpb->bytes_per_sector;
    fs->sectors_per_cluster = bpb->sectors_per_cluster;
    fs->cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    fs->reserved_sector_count = bpb->reserved_sector_count;
    fs->num_fats = bpb->num_fats;
    fs->fat_size_sectors = geo.fat_size;
    fs->fat_start_lba = bpb->reserved_sector_count;
    fs->root_dir_lba = fs->fat_start_lba + fs->num_fats * fs->fat_size_sectors;
    fs->root_dir_sectors = geo.root_dir_sectors;
    fs->data_start_lba = geo.data_start;
    fs->total_sectors = geo.total_sectors;
    fs->total_clusters = geo.total_clusters;
    fs->next_free_hint = 2;

    const char *label_src;
    if (fs->fat_bits == 32) {
        fs->root_cluster = bpb->root_cluster;
        fs->fsinfo_sector = (bpb->fs_info_sector != 0xFFFF) ? bpb->fs_info_sector : 0;
        label_src = bpb->volume_label;
    } else {
        fs->root_cluster = FAT_ROOT_PSEUDO_BASE;
        fs->fsinfo_sector = 0;
        label_src = (buf[38] == 0x29) ? (const char *)(buf + 43) : "NO NAME    ";
    }

    memcpy(fs->label, label_src, 11);
    fs->label[11] = '\0';
    for (int i = 10; i >= 0; i--) { if (fs->label[i] == ' ') fs->label[i] = '\0'; else break; }

    kfree(buf);

    out->type = (fs->fat_bits == 12) ? FS_TYPE_FAT12 : (fs->fat_bits == 16) ? FS_TYPE_FAT16 : FS_TYPE_FAT32;
    out->dev = dev;
    out->ops = &fat32_ops;
    out->priv = fs;
    strncpy(out->label, fs->label, sizeof(out->label) - 1);
    out->label[sizeof(out->label) - 1] = '\0';

    LOG_DEBUG("mounted FAT%u volume '%s', cluster=%u bytes, clusters=%u",
         (unsigned)fs->fat_bits, fs->label[0] ? fs->label : "(no label)",
         (unsigned)fs->cluster_size, (unsigned)fs->total_clusters);

    return FS_OK;
}

#define FAT_FORMAT_MAX_CLUSTER_BYTES 32768u

static void fat_label_field(const char *label, char out[11]) {
    memset(out, ' ', 11);
    if (!label || !label[0]) {
        memcpy(out, "NO NAME    ", 11);
        return;
    }
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)label; *p && n < 11; p++) {
        unsigned char c = *p;
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        if (c < 0x20 || c >= 0x7F || strchr("\"*+,./:;<=>?[\\]|", c)) c = '_';
        out[n++] = (char)c;
    }
}

static uint32_t fat_format_layout(uint8_t bits, uint32_t total, uint32_t bps, uint32_t spc,
                                  uint32_t reserved, uint32_t root_secs, uint32_t *fat_size) {
    uint32_t fsz = 1, clusters = 0;
    for (int it = 0; it < 64; it++) {
        uint64_t meta = reserved + 2ull * fsz + root_secs;
        if (meta >= total) return 0;
        clusters = (uint32_t)((total - meta) / spc);
        uint64_t bytes = (bits == 12) ? ((uint64_t)(clusters + 2) * 3 + 1) / 2
                                      : (uint64_t)(clusters + 2) * (bits / 8u);
        uint32_t need = (uint32_t)((bytes + bps - 1) / bps);
        if (need <= fsz) break;
        fsz = need;
    }
    *fat_size = fsz;
    return clusters;
}

int fat_format(struct block_device *dev, enum fs_type type, const char *label, uint64_t hidden_sectors) {
    if (!dev) return FS_ERR_PARAM;
    uint32_t bps = dev->sector_size ? dev->sector_size : 512;
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) return FS_ERR_NOSUPP;

    uint8_t bits = (type == FS_TYPE_FAT12) ? 12 : (type == FS_TYPE_FAT16) ? 16 : 32;
    uint32_t total = dev->sector_count > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)dev->sector_count;
    uint32_t reserved = (bits == 32) ? 32u : 1u;
    uint32_t root_entries = (bits == 32) ? 0u : 512u;
    uint32_t root_secs = (root_entries * 32u + bps - 1u) / bps;
    uint32_t min_c = (bits == 12) ? 1u : (bits == 16) ? 4085u : 65525u;
    uint32_t max_c = (bits == 12) ? 4084u : (bits == 16) ? 65524u : 0x0FFFFFF4u;
    uint32_t max_spc = FAT_FORMAT_MAX_CLUSTER_BYTES / bps;

    uint32_t spc = 1;
    if (bits == 32) {
        uint64_t bytes = (uint64_t)total * bps;
        uint32_t cluster_bytes = bytes <= 260ull * 1024 * 1024 ? 512u :
                                 bytes <= 8ull * 1024 * 1024 * 1024 ? 4096u :
                                 bytes <= 16ull * 1024 * 1024 * 1024 ? 8192u :
                                 bytes <= 32ull * 1024 * 1024 * 1024 ? 16384u : 32768u;
        spc = cluster_bytes > bps ? cluster_bytes / bps : 1u;
    }

    uint32_t fat_size = 0, clusters = 0;
    for (;;) {
        clusters = fat_format_layout(bits, total, bps, spc, reserved, root_secs, &fat_size);
        if (clusters > max_c) {
            if (spc >= max_spc) {
                LOG_ERROR("%s: volume is too large for FAT%u", dev->name, (unsigned)bits);
                return FS_ERR_NOSUPP;
            }
            spc <<= 1;
            continue;
        }
        if (clusters < min_c && bits == 32 && spc > 1) {
            spc >>= 1;
            continue;
        }
        break;
    }
    if (clusters < min_c || clusters == 0) {
        LOG_ERROR("%s: volume is too small for FAT%u", dev->name, (unsigned)bits);
        return FS_ERR_NOSPACE;
    }

    uint32_t data_start = reserved + 2u * fat_size + root_secs;
    char label11[11];
    fat_label_field(label, label11);
    uint32_t serial = fs_new_serial();

    LOG_INFO("%s: creating FAT%u, %u clusters of %u bytes, FAT %u sectors",
             dev->name, (unsigned)bits, clusters, spc * bps, fat_size);

    uint8_t *buf = kmalloc(bps);
    if (!buf) return FS_ERR_NOMEM;

    int r = fs_dev_wipe(dev);
    if (r == FS_OK) r = fs_dev_zero(dev, 0, data_start + (bits == 32 ? spc : 0u));

    for (uint32_t f = 0; f < 2 && r == FS_OK; f++) {
        memset(buf, 0, bps);
        if (bits == 12) {
            buf[0] = 0xF8; buf[1] = 0xFF; buf[2] = 0xFF;
        } else if (bits == 16) {
            buf[0] = 0xF8; buf[1] = 0xFF; buf[2] = 0xFF; buf[3] = 0xFF;
        } else {
            uint32_t e[3] = { 0x0FFFFFF8u, 0x0FFFFFFFu, 0x0FFFFFFFu };
            memcpy(buf, e, sizeof(e));
        }
        if (dev->write_sectors(dev, reserved + f * fat_size, 1, buf) != 0) r = FS_ERR_IO;
    }

    if (r == FS_OK && label && label[0]) {
        memset(buf, 0, bps);
        fat32_dirent_raw_t *vol = (fat32_dirent_raw_t *)buf;
        memcpy(vol->name, label11, 11);
        vol->attr = 0x08;
        vol->write_date = 0x21;
        uint32_t root_lba = reserved + 2u * fat_size;
        if (dev->write_sectors(dev, root_lba, 1, buf) != 0) r = FS_ERR_IO;
    }

    if (r == FS_OK && bits == 32) {
        memset(buf, 0, bps);
        uint32_t lead = 0x41615252u, mid = 0x61417272u, free_count = clusters - 1u, next = 3u, trail = 0xAA550000u;
        memcpy(buf + 0, &lead, 4);
        memcpy(buf + 484, &mid, 4);
        memcpy(buf + 488, &free_count, 4);
        memcpy(buf + 492, &next, 4);
        memcpy(buf + 508, &trail, 4);
        if (dev->write_sectors(dev, 1, 1, buf) != 0 || dev->write_sectors(dev, 7, 1, buf) != 0) r = FS_ERR_IO;
    }

    if (r == FS_OK) {
        memset(buf, 0, bps);
        fat32_bpb_t *bpb = (fat32_bpb_t *)buf;
        bpb->jmp[0] = 0xEB;
        bpb->jmp[1] = (bits == 32) ? 0x58 : 0x3C;
        bpb->jmp[2] = 0x90;
        memcpy(bpb->oem_name, "LUOS    ", 8);
        bpb->bytes_per_sector = (uint16_t)bps;
        bpb->sectors_per_cluster = (uint8_t)spc;
        bpb->reserved_sector_count = (uint16_t)reserved;
        bpb->num_fats = 2;
        bpb->root_entry_count = (uint16_t)root_entries;
        bpb->media = 0xF8;
        bpb->sectors_per_track = 63;
        bpb->num_heads = 255;
        bpb->hidden_sectors = hidden_sectors > 0xFFFFFFFFull ? 0u : (uint32_t)hidden_sectors;
        if (bits != 32 && total < 65536u) bpb->total_sectors_16 = (uint16_t)total;
        else bpb->total_sectors_32 = total;

        uint32_t code_off;
        if (bits == 32) {
            bpb->fat_size_32 = fat_size;
            bpb->root_cluster = 2;
            bpb->fs_info_sector = 1;
            bpb->backup_boot_sector = 6;
            bpb->drive_number = 0x80;
            bpb->boot_signature = 0x29;
            bpb->volume_id = serial;
            memcpy(bpb->volume_label, label11, 11);
            memcpy(bpb->fs_type, "FAT32   ", 8);
            code_off = 0x5A;
        } else {
            bpb->fat_size_16 = (uint16_t)fat_size;
            buf[36] = 0x80;
            buf[38] = 0x29;
            memcpy(buf + 39, &serial, 4);
            memcpy(buf + 43, label11, 11);
            memcpy(buf + 54, bits == 12 ? "FAT12   " : "FAT16   ", 8);
            code_off = 0x3E;
        }
        buf[code_off] = 0xFA;
        buf[code_off + 1] = 0xF4;
        buf[code_off + 2] = 0xEB;
        buf[code_off + 3] = 0xFD;
        buf[FAT32_BOOT_SIG_OFFSET] = FAT32_BOOT_SIG_0;
        buf[FAT32_BOOT_SIG_OFFSET + 1] = FAT32_BOOT_SIG_1;

        if (bits == 32 && dev->write_sectors(dev, 6, 1, buf) != 0) r = FS_ERR_IO;
        if (r == FS_OK && dev->write_sectors(dev, 0, 1, buf) != 0) r = FS_ERR_IO;
    }

    kfree(buf);
    return r;
}
