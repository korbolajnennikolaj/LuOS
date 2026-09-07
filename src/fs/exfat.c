#include "fs/exfat.h"

#include "components/Memory/heap.h"

#include <stdio.h>
#include <string.h>

#define XLOG(fmt, ...) printf_color(0xFF55FFFF, "[exfat] " fmt, ##__VA_ARGS__)
#define XERR(fmt, ...) printf_color(0xFFFF5555, "[exfat] ERR " fmt, ##__VA_ARGS__)

#define EXFAT_CLUSTER_EOC_MIN 0xFFFFFFF8u
#define EXFAT_CLUSTER_BAD 0xFFFFFFF7u

static inline uint64_t cluster_to_lba(exfat_fs_t *fs, uint32_t cluster) {
    return (uint64_t)fs->cluster_heap_offset_lba + (uint64_t)(cluster - 2) * fs->sectors_per_cluster;
}

static int read_cluster(exfat_fs_t *fs, uint32_t cluster, void *buf) {
    if (cluster < 2) return FS_ERR_PARAM;
    if (fs->dev->read_sectors(fs->dev, cluster_to_lba(fs, cluster), fs->sectors_per_cluster, buf) != 0)
        return FS_ERR_IO;
    return FS_OK;
}

static int write_cluster(exfat_fs_t *fs, uint32_t cluster, const void *buf) {
    if (cluster < 2) return FS_ERR_PARAM;
    if (fs->dev->write_sectors(fs->dev, cluster_to_lba(fs, cluster), fs->sectors_per_cluster, (void *)buf) != 0)
        return FS_ERR_IO;
    return FS_OK;
}

static uint32_t get_fat_entry(exfat_fs_t *fs, uint32_t cluster) {
    uint32_t byte_off = cluster * 4;
    uint32_t sector = fs->fat_offset_lba + (byte_off / fs->bytes_per_sector);
    uint32_t off = byte_off % fs->bytes_per_sector;

    uint8_t *buf = kmalloc(fs->bytes_per_sector);
    if (!buf) return EXFAT_CLUSTER_EOC_MIN;
    if (fs->dev->read_sectors(fs->dev, sector, 1, buf) != 0) { kfree(buf); return EXFAT_CLUSTER_EOC_MIN; }
    uint32_t raw;
    memcpy(&raw, buf + off, 4);
    kfree(buf);
    return raw;
}

static uint32_t cluster_for_index(exfat_fs_t *fs, uint32_t first_cluster, bool no_fat_chain,
                                   uint32_t *cache_index, uint32_t *cache_cluster, uint32_t index) {
    if (first_cluster < 2) return 0;

    if (no_fat_chain) {
        uint32_t c = first_cluster + index;
        if (c < 2 || (c - 2) >= fs->cluster_count) return 0;
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
        memcpy(scratch + off, in + done, (size_t)chunk);
        if (write_cluster(fs, phys, scratch) != FS_OK) break;
        done += chunk;
    }

    kfree(scratch);
    return (int64_t)done;
}

static void utf16_to_utf8(const uint16_t *src, uint32_t src_len, char *dst, size_t dst_cap) {
    size_t o = 0;
    for (uint32_t i = 0; i < src_len && o + 3 < dst_cap; i++) {
        uint16_t c = src[i];
        if (c < 0x80) {
            dst[o++] = (char)c;
        } else if (c < 0x800) {
            dst[o++] = (char)(0xC0 | (c >> 6));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            dst[o++] = (char)(0xE0 | (c >> 12));
            dst[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    dst[o] = '\0';
}

static bool name_matches(const char *utf8_name, const uint16_t *utf16_name, uint32_t utf16_len) {
    size_t i = 0;
    for (uint32_t j = 0; j < utf16_len; j++) {
        char c8 = utf8_name[i];
        if (c8 == '\0') return false;
        uint16_t c16 = utf16_name[j];
        if (c16 > 0x7F) return false;
        char a = c8, b = (char)c16;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
        i++;
    }
    return utf8_name[i] == '\0';
}

typedef struct {
    uint16_t attributes;
    uint32_t first_cluster;
    bool no_fat_chain;
    uint64_t data_length;
    char name_utf8[FS_MAX_NAME + 1];
} exfat_entry_info_t;

static int dir_find(exfat_fs_t *fs, uint32_t dir_first_cluster, bool dir_no_fat_chain,
                     const char *name, exfat_entry_info_t *out) {
    uint8_t entry[32];
    uint32_t cache_index = 0, cache_cluster = dir_first_cluster;
    uint64_t pos = 0;
    int ret = FS_ERR_NOENT;

    for (;;) {
        int64_t n = stream_read(fs, dir_first_cluster, dir_no_fat_chain, &cache_index, &cache_cluster,
                                 pos, entry, 32);
        if (n != 32) break;
        pos += 32;

        uint8_t type = entry[0];
        if (type == EXFAT_ENTRY_EOD) break;
        if (type != EXFAT_ENTRY_TYPE_FILE) continue;

        exfat_file_entry_t fe;
        memcpy(&fe, entry, sizeof(fe));
        if (fe.secondary_count < 1 || fe.secondary_count > 18) { continue; }

        uint8_t stream_raw[32];
        int64_t n2 = stream_read(fs, dir_first_cluster, dir_no_fat_chain, &cache_index, &cache_cluster,
                                  pos, stream_raw, 32);
        if (n2 != 32 || stream_raw[0] != EXFAT_ENTRY_TYPE_STREAM) {
            pos += (uint64_t)fe.secondary_count * 32;
            continue;
        }
        exfat_stream_entry_t se;
        memcpy(&se, stream_raw, sizeof(se));
        pos += 32;

        uint16_t namebuf[256];
        uint32_t got = 0;
        uint8_t remaining = (uint8_t)(fe.secondary_count - 1);
        for (uint8_t k = 0; k < remaining && got < se.name_length; k++) {
            uint8_t nraw[32];
            int64_t n3 = stream_read(fs, dir_first_cluster, dir_no_fat_chain, &cache_index, &cache_cluster,
                                      pos, nraw, 32);
            pos += 32;
            if (n3 != 32 || nraw[0] != EXFAT_ENTRY_TYPE_FILENAME) continue;
            exfat_filename_entry_t ne;
            memcpy(&ne, nraw, sizeof(ne));
            for (int c = 0; c < 15 && got < se.name_length; c++) {
                namebuf[got++] = ne.name[c];
            }
        }

        if (name_matches(name, namebuf, got)) {
            out->attributes = fe.file_attributes;
            out->first_cluster = se.first_cluster;
            out->no_fat_chain = (se.general_flags & EXFAT_STREAM_FLAG_NOFATCHAIN) != 0;
            out->data_length = se.data_length;
            utf16_to_utf8(namebuf, got, out->name_utf8, sizeof(out->name_utf8));
            ret = FS_OK;
            break;
        }
    }

    return ret;
}

static int resolve_path(exfat_fs_t *fs, const char *path, exfat_entry_info_t *out) {
    fs_path_parts_t pp;
    int r = fs_split_path(path, &pp);
    if (r != FS_OK) return r;

    if (pp.count == 0) {
        out->attributes = EXFAT_ATTR_DIRECTORY;
        out->first_cluster = fs->root_cluster;
        out->no_fat_chain = false;
        out->data_length = 0;
        out->name_utf8[0] = '\0';
        return FS_OK;
    }

    uint32_t cur_cluster = fs->root_cluster;
    bool cur_no_fat_chain = false;
    exfat_entry_info_t info = {0};

    for (int i = 0; i < pp.count; i++) {
        r = dir_find(fs, cur_cluster, cur_no_fat_chain, pp.comps[i], &info);
        if (r != FS_OK) return r;
        if (i != pp.count - 1 && !(info.attributes & EXFAT_ATTR_DIRECTORY)) return FS_ERR_NOTDIR;
        cur_cluster = info.first_cluster;
        cur_no_fat_chain = info.no_fat_chain;
    }

    *out = info;
    return FS_OK;
}

static int exfat_op_open(fs_t *fsroot, const char *path, bool create, bool truncate_flag, fs_file_t *out) {
    (void)truncate_flag;
    exfat_fs_t *fs = (exfat_fs_t *)fsroot->priv;
    if (create) return FS_ERR_NOSUPP;

    exfat_entry_info_t info;
    int r = resolve_path(fs, path, &info);
    if (r != FS_OK) return r;
    if (info.attributes & EXFAT_ATTR_DIRECTORY) return FS_ERR_ISDIR;

    exfat_file_t *ef = kmalloc(sizeof(exfat_file_t));
    if (!ef) return FS_ERR_NOMEM;
    ef->fs = fs;
    ef->first_cluster = info.first_cluster;
    ef->no_fat_chain = info.no_fat_chain;
    ef->size = info.data_length;
    ef->cache_index = 0;
    ef->cache_cluster = info.first_cluster;

    out->priv = ef;
    out->size = ef->size;
    out->pos = 0;
    out->writable = true;
    return FS_OK;
}

static int exfat_op_close(fs_file_t *f) {
    if (f->priv) kfree(f->priv);
    f->priv = NULL;
    return FS_OK;
}

static int64_t exfat_op_read(fs_file_t *f, void *buf, uint64_t size) {
    exfat_file_t *ef = (exfat_file_t *)f->priv;
    if (f->pos >= ef->size) return 0;
    uint64_t remaining = ef->size - f->pos;
    if (size > remaining) size = remaining;

    int64_t n = stream_read(ef->fs, ef->first_cluster, ef->no_fat_chain,
                             &ef->cache_index, &ef->cache_cluster, f->pos, buf, size);
    if (n > 0) f->pos += (uint64_t)n;
    return n;
}

static int64_t exfat_op_write(fs_file_t *f, const void *buf, uint64_t size) {
    exfat_file_t *ef = (exfat_file_t *)f->priv;
    if (f->pos >= ef->size) return 0;
    uint64_t remaining = ef->size - f->pos;
    if (size > remaining) size = remaining;

    int64_t n = stream_write(ef->fs, ef->first_cluster, ef->no_fat_chain,
                              &ef->cache_index, &ef->cache_cluster, f->pos, buf, size);
    if (n > 0) f->pos += (uint64_t)n;
    return n;
}

static int exfat_op_seek(fs_file_t *f, uint64_t pos) { f->pos = pos; return FS_OK; }
static int exfat_op_truncate(fs_file_t *f, uint64_t size) { (void)f; (void)size; return FS_ERR_NOSUPP; }

static int exfat_op_opendir(fs_t *fsroot, const char *path, fs_dir_t *out) {
    exfat_fs_t *fs = (exfat_fs_t *)fsroot->priv;

    exfat_entry_info_t info;
    int r = resolve_path(fs, path, &info);
    if (r != FS_OK) return r;
    if (!(info.attributes & EXFAT_ATTR_DIRECTORY)) return FS_ERR_NOTDIR;

    exfat_diriter_t *it = kmalloc(sizeof(exfat_diriter_t));
    if (!it) return FS_ERR_NOMEM;
    it->fs = fs;
    it->dir_first_cluster = info.first_cluster;
    it->dir_no_fat_chain = info.no_fat_chain;
    it->pos = 0;
    it->cache_index = 0;
    it->cache_cluster = info.first_cluster;
    it->loaded_cluster_idx = (uint32_t)-1;
    it->cluster_buf = kmalloc(fs->cluster_size);
    if (!it->cluster_buf) { kfree(it); return FS_ERR_NOMEM; }

    out->priv = it;
    return FS_OK;
}

static int exfat_op_readdir(fs_dir_t *dir, fs_dirent_t *out) {
    exfat_diriter_t *it = (exfat_diriter_t *)dir->priv;
    exfat_fs_t *fs = it->fs;

    for (;;) {
        uint8_t entry[32];
        int64_t n = stream_read(fs, it->dir_first_cluster, it->dir_no_fat_chain,
                                 &it->cache_index, &it->cache_cluster, it->pos, entry, 32);
        if (n != 32) return FS_ERR_EOF;
        it->pos += 32;

        uint8_t type = entry[0];
        if (type == EXFAT_ENTRY_EOD) return FS_ERR_EOF;
        if (type != EXFAT_ENTRY_TYPE_FILE) continue;

        exfat_file_entry_t fe;
        memcpy(&fe, entry, sizeof(fe));
        if (fe.secondary_count < 1 || fe.secondary_count > 18) continue;

        uint8_t stream_raw[32];
        int64_t n2 = stream_read(fs, it->dir_first_cluster, it->dir_no_fat_chain,
                                  &it->cache_index, &it->cache_cluster, it->pos, stream_raw, 32);
        if (n2 != 32 || stream_raw[0] != EXFAT_ENTRY_TYPE_STREAM) {
            it->pos += (uint64_t)fe.secondary_count * 32;
            continue;
        }
        exfat_stream_entry_t se;
        memcpy(&se, stream_raw, sizeof(se));
        it->pos += 32;

        uint16_t namebuf[256];
        uint32_t got = 0;
        uint8_t remaining = (uint8_t)(fe.secondary_count - 1);
        for (uint8_t k = 0; k < remaining && got < se.name_length; k++) {
            uint8_t nraw[32];
            int64_t n3 = stream_read(fs, it->dir_first_cluster, it->dir_no_fat_chain,
                                      &it->cache_index, &it->cache_cluster, it->pos, nraw, 32);
            it->pos += 32;
            if (n3 != 32 || nraw[0] != EXFAT_ENTRY_TYPE_FILENAME) continue;
            exfat_filename_entry_t ne;
            memcpy(&ne, nraw, sizeof(ne));
            for (int c = 0; c < 15 && got < se.name_length; c++) {
                namebuf[got++] = ne.name[c];
            }
        }

        utf16_to_utf8(namebuf, got, out->name, sizeof(out->name));
        out->type = (fe.file_attributes & EXFAT_ATTR_DIRECTORY) ? FS_ENTRY_DIR : FS_ENTRY_FILE;
        out->size = se.data_length;
        return FS_OK;
    }
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
    r = resolve_path(fs, path, &info);
    if (r != FS_OK) return r;

    const char *name = pp.count ? pp.comps[pp.count - 1] : "/";
    strncpy(out->name, name, FS_MAX_NAME); out->name[FS_MAX_NAME] = '\0';
    out->type = (info.attributes & EXFAT_ATTR_DIRECTORY) ? FS_ENTRY_DIR : FS_ENTRY_FILE;
    out->size = info.data_length;
    return FS_OK;
}

static int exfat_op_notsupported_path(fs_t *fsroot, const char *path) { (void)fsroot; (void)path; return FS_ERR_NOSUPP; }

static int exfat_op_unmount(fs_t *fsroot) {
    if (fsroot->priv) kfree(fsroot->priv);
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
    .mkdir = exfat_op_notsupported_path,
    .unlink = exfat_op_notsupported_path,
    .rmdir = exfat_op_notsupported_path,
    .stat = exfat_op_stat,
    .unmount = exfat_op_unmount,
};

static int load_bootsector(struct block_device *dev, exfat_bootsector_t *out) {
    uint32_t sector_size = dev->sector_size ? dev->sector_size : 512;
    uint8_t *buf = kmalloc(sector_size);
    if (!buf) return FS_ERR_NOMEM;
    if (dev->read_sectors(dev, 0, 1, buf) != 0) { kfree(buf); return FS_ERR_IO; }

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

static void load_volume_label(exfat_fs_t *fs) {
    fs->label[0] = '\0';
    uint8_t entry[32];
    uint32_t cache_index = 0, cache_cluster = fs->root_cluster;
    uint64_t pos = 0;

    for (int guard = 0; guard < 4096; guard++) {
        int64_t n = stream_read(fs, fs->root_cluster, false, &cache_index, &cache_cluster, pos, entry, 32);
        if (n != 32) break;
        pos += 32;
        if (entry[0] == EXFAT_ENTRY_EOD) break;
        if (entry[0] == EXFAT_ENTRY_TYPE_LABEL) {
            uint8_t char_count = entry[1];
            if (char_count > 11) char_count = 11;
            uint16_t namebuf[11];
            memcpy(namebuf, entry + 2, (size_t)char_count * 2);
            utf16_to_utf8(namebuf, char_count, fs->label, sizeof(fs->label));
            break;
        }
    }
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
    fs->cluster_heap_offset_lba = bs.cluster_heap_offset;
    fs->cluster_count = bs.cluster_count;
    fs->root_cluster = bs.first_cluster_of_root;

    if (fs->bytes_per_sector < 512 || fs->cluster_size == 0 || fs->root_cluster < 2) {
        kfree(fs);
        XERR("invalid boot sector fields\n");
        return FS_ERR_CORRUPT;
    }

    load_volume_label(fs);

    out->type = FS_TYPE_EXFAT;
    out->dev = dev;
    out->ops = &exfat_ops;
    out->priv = fs;
    strncpy(out->label, fs->label[0] ? fs->label : "", sizeof(out->label) - 1);
    out->label[sizeof(out->label) - 1] = '\0';

    XLOG("mounted volume '%s', cluster=%u bytes, clusters=%u\n",
         fs->label[0] ? fs->label : "(no label)",
         (unsigned)fs->cluster_size, (unsigned)fs->cluster_count);

    return FS_OK;
}
