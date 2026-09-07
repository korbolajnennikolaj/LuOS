#include "fs/iso9660.h"

#include "components/Memory/heap.h"

#include <stdio.h>
#include <string.h>

#define ILOG(fmt, ...) printf_color(0xFF55FFFF, "[iso9660] " fmt, ##__VA_ARGS__)
#define IERR(fmt, ...) printf_color(0xFFFF5555, "[iso9660] ERR " fmt, ##__VA_ARGS__)

static int read_iso_block(iso9660_fs_t *fs, uint32_t iso_lba, void *buf) {
    uint64_t dev_lba = (uint64_t)iso_lba * fs->sectors_per_block;
    if (fs->dev->read_sectors(fs->dev, dev_lba, fs->sectors_per_block, buf) != 0)
        return FS_ERR_IO;
    return FS_OK;
}

static int64_t extent_read(iso9660_fs_t *fs, uint32_t extent_lba, uint32_t file_size,
                            uint64_t pos, void *out_buf, uint64_t size) {
    if (pos >= file_size) return 0;
    uint64_t remaining = file_size - pos;
    if (size > remaining) size = remaining;

    uint8_t *scratch = kmalloc(ISO9660_BLOCK_SIZE);
    if (!scratch) return FS_ERR_NOMEM;

    uint8_t *out = (uint8_t *)out_buf;
    uint64_t done = 0;
    while (done < size) {
        uint32_t blk = (uint32_t)((pos + done) / ISO9660_BLOCK_SIZE);
        uint32_t off = (uint32_t)((pos + done) % ISO9660_BLOCK_SIZE);
        uint64_t chunk = ISO9660_BLOCK_SIZE - off;
        if (chunk > size - done) chunk = size - done;

        if (read_iso_block(fs, extent_lba + blk, scratch) != FS_OK) break;
        memcpy(out + done, scratch + off, (size_t)chunk);
        done += chunk;
    }

    kfree(scratch);
    return (int64_t)done;
}

static int read_whole_dir(iso9660_fs_t *fs, uint32_t extent_lba, uint32_t data_length,
                           uint8_t **out_buf, uint32_t *out_buf_len) {
    uint32_t buf_len = (data_length + ISO9660_BLOCK_SIZE - 1) & ~(ISO9660_BLOCK_SIZE - 1);
    if (buf_len == 0) buf_len = ISO9660_BLOCK_SIZE;

    uint8_t *buf = kmalloc(buf_len);
    if (!buf) return FS_ERR_NOMEM;

    uint32_t blocks = buf_len / ISO9660_BLOCK_SIZE;
    for (uint32_t i = 0; i < blocks; i++) {
        if (read_iso_block(fs, extent_lba + i, buf + (uint64_t)i * ISO9660_BLOCK_SIZE) != FS_OK) {
            kfree(buf);
            return FS_ERR_IO;
        }
    }

    *out_buf = buf;
    *out_buf_len = buf_len;
    return FS_OK;
}

static void clean_iso_name(const char *raw, uint8_t raw_len, char *out, size_t out_cap) {
    uint8_t n = raw_len;
    for (uint8_t i = 0; i < raw_len; i++) {
        if (raw[i] == ';') { n = i; break; }
    }
    if (n > 0 && raw[n - 1] == '.' && !(n == 1)) n--;
    if (n >= out_cap) n = (uint8_t)(out_cap - 1);
    memcpy(out, raw, n);
    out[n] = '\0';
}

static bool ascii_ieq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

typedef struct {
    uint32_t extent_lba;
    uint32_t data_length;
    bool is_dir;
    char name_utf8[FS_MAX_NAME + 1];
} iso9660_entry_info_t;

static int dir_find(iso9660_fs_t *fs, uint32_t dir_extent_lba, uint32_t dir_data_length,
                     const char *name, iso9660_entry_info_t *out) {
    uint8_t *buf; uint32_t buf_len;
    int r = read_whole_dir(fs, dir_extent_lba, dir_data_length, &buf, &buf_len);
    if (r != FS_OK) return r;

    int ret = FS_ERR_NOENT;
    uint32_t off = 0;
    while (off < dir_data_length) {
        uint8_t reclen = buf[off];
        if (reclen == 0) {

            off = (off + ISO9660_BLOCK_SIZE) & ~(ISO9660_BLOCK_SIZE - 1);
            continue;
        }

        iso9660_dirrecord_t rec;
        memcpy(&rec, buf + off, sizeof(rec));
        const char *ident = (const char *)(buf + off + sizeof(iso9660_dirrecord_t));

        if (rec.name_len == 1 && (ident[0] == 0x00 || ident[0] == 0x01)) {

        } else {
            char cleaned[FS_MAX_NAME + 1];
            clean_iso_name(ident, rec.name_len, cleaned, sizeof(cleaned));
            if (ascii_ieq(cleaned, name)) {
                out->extent_lba = rec.extent_lba_le;
                out->data_length = rec.data_len_le;
                out->is_dir = (rec.file_flags & ISO9660_FLAG_DIRECTORY) != 0;
                strncpy(out->name_utf8, cleaned, FS_MAX_NAME);
                out->name_utf8[FS_MAX_NAME] = '\0';
                ret = FS_OK;
                break;
            }
        }

        off += reclen;
    }

    kfree(buf);
    return ret;
}

static int resolve_path(iso9660_fs_t *fs, const char *path, iso9660_entry_info_t *out) {
    fs_path_parts_t pp;
    int r = fs_split_path(path, &pp);
    if (r != FS_OK) return r;

    if (pp.count == 0) {
        out->extent_lba = fs->root_extent_lba;
        out->data_length = fs->root_data_length;
        out->is_dir = true;
        out->name_utf8[0] = '\0';
        return FS_OK;
    }

    uint32_t cur_lba = fs->root_extent_lba;
    uint32_t cur_len = fs->root_data_length;
    iso9660_entry_info_t info = {0};

    for (int i = 0; i < pp.count; i++) {
        r = dir_find(fs, cur_lba, cur_len, pp.comps[i], &info);
        if (r != FS_OK) return r;
        if (i != pp.count - 1 && !info.is_dir) return FS_ERR_NOTDIR;
        cur_lba = info.extent_lba;
        cur_len = info.data_length;
    }

    *out = info;
    return FS_OK;
}

static int iso9660_op_open(fs_t *fsroot, const char *path, bool create, bool truncate_flag, fs_file_t *out) {
    (void)truncate_flag;
    iso9660_fs_t *fs = (iso9660_fs_t *)fsroot->priv;
    if (create) return FS_ERR_NOSUPP;

    iso9660_entry_info_t info;
    int r = resolve_path(fs, path, &info);
    if (r != FS_OK) return r;
    if (info.is_dir) return FS_ERR_ISDIR;

    iso9660_file_t *ifile = kmalloc(sizeof(iso9660_file_t));
    if (!ifile) return FS_ERR_NOMEM;
    ifile->fs = fs;
    ifile->extent_lba = info.extent_lba;
    ifile->size = info.data_length;

    out->priv = ifile;
    out->size = ifile->size;
    out->pos = 0;
    out->writable = false;
    return FS_OK;
}

static int iso9660_op_close(fs_file_t *f) {
    if (f->priv) kfree(f->priv);
    f->priv = NULL;
    return FS_OK;
}

static int64_t iso9660_op_read(fs_file_t *f, void *buf, uint64_t size) {
    iso9660_file_t *ifile = (iso9660_file_t *)f->priv;
    int64_t n = extent_read(ifile->fs, ifile->extent_lba, ifile->size, f->pos, buf, size);
    if (n > 0) f->pos += (uint64_t)n;
    return n;
}

static int64_t iso9660_op_write(fs_file_t *f, const void *buf, uint64_t size) {
    (void)f; (void)buf; (void)size;
    return FS_ERR_NOSUPP;
}

static int iso9660_op_seek(fs_file_t *f, uint64_t pos) { f->pos = pos; return FS_OK; }
static int iso9660_op_truncate(fs_file_t *f, uint64_t size) { (void)f; (void)size; return FS_ERR_NOSUPP; }

static int iso9660_op_opendir(fs_t *fsroot, const char *path, fs_dir_t *out) {
    iso9660_fs_t *fs = (iso9660_fs_t *)fsroot->priv;

    iso9660_entry_info_t info;
    int r = resolve_path(fs, path, &info);
    if (r != FS_OK) return r;
    if (!info.is_dir) return FS_ERR_NOTDIR;

    iso9660_diriter_t *it = kmalloc(sizeof(iso9660_diriter_t));
    if (!it) return FS_ERR_NOMEM;
    it->fs = fs;
    it->data_len = info.data_length;
    it->offset = 0;
    r = read_whole_dir(fs, info.extent_lba, info.data_length, &it->buf, &it->buf_len);
    if (r != FS_OK) { kfree(it); return r; }

    out->priv = it;
    return FS_OK;
}

static int iso9660_op_readdir(fs_dir_t *dir, fs_dirent_t *out) {
    iso9660_diriter_t *it = (iso9660_diriter_t *)dir->priv;

    while (it->offset < it->data_len) {
        uint8_t reclen = it->buf[it->offset];
        if (reclen == 0) {
            it->offset = (it->offset + ISO9660_BLOCK_SIZE) & ~(ISO9660_BLOCK_SIZE - 1);
            continue;
        }

        iso9660_dirrecord_t rec;
        memcpy(&rec, it->buf + it->offset, sizeof(rec));
        const char *ident = (const char *)(it->buf + it->offset + sizeof(iso9660_dirrecord_t));
        it->offset += reclen;

        if (rec.name_len == 1 && (ident[0] == 0x00 || ident[0] == 0x01)) continue;

        char cleaned[FS_MAX_NAME + 1];
        clean_iso_name(ident, rec.name_len, cleaned, sizeof(cleaned));
        strncpy(out->name, cleaned, FS_MAX_NAME); out->name[FS_MAX_NAME] = '\0';
        out->type = (rec.file_flags & ISO9660_FLAG_DIRECTORY) ? FS_ENTRY_DIR : FS_ENTRY_FILE;
        out->size = rec.data_len_le;
        return FS_OK;
    }
    return FS_ERR_EOF;
}

static int iso9660_op_closedir(fs_dir_t *dir) {
    iso9660_diriter_t *it = (iso9660_diriter_t *)dir->priv;
    if (it) {
        if (it->buf) kfree(it->buf);
        kfree(it);
    }
    dir->priv = NULL;
    return FS_OK;
}

static int iso9660_op_stat(fs_t *fsroot, const char *path, fs_dirent_t *out) {
    iso9660_fs_t *fs = (iso9660_fs_t *)fsroot->priv;

    fs_path_parts_t pp;
    int r = fs_split_path(path, &pp);
    if (r != FS_OK) return r;

    iso9660_entry_info_t info;
    r = resolve_path(fs, path, &info);
    if (r != FS_OK) return r;

    const char *name = pp.count ? pp.comps[pp.count - 1] : "/";
    strncpy(out->name, name, FS_MAX_NAME); out->name[FS_MAX_NAME] = '\0';
    out->type = info.is_dir ? FS_ENTRY_DIR : FS_ENTRY_FILE;
    out->size = info.data_length;
    return FS_OK;
}

static int iso9660_op_notsupported_path(fs_t *fsroot, const char *path) { (void)fsroot; (void)path; return FS_ERR_NOSUPP; }

static int iso9660_op_unmount(fs_t *fsroot) {
    if (fsroot->priv) kfree(fsroot->priv);
    fsroot->priv = NULL;
    return FS_OK;
}

const fs_ops_t iso9660_ops = {
    .open = iso9660_op_open,
    .close = iso9660_op_close,
    .read = iso9660_op_read,
    .write = iso9660_op_write,
    .seek = iso9660_op_seek,
    .truncate = iso9660_op_truncate,
    .opendir = iso9660_op_opendir,
    .readdir = iso9660_op_readdir,
    .closedir = iso9660_op_closedir,
    .mkdir = iso9660_op_notsupported_path,
    .unlink = iso9660_op_notsupported_path,
    .rmdir = iso9660_op_notsupported_path,
    .stat = iso9660_op_stat,
    .unmount = iso9660_op_unmount,
};

static int find_pvd(struct block_device *dev, uint32_t sectors_per_block, uint8_t *out_2048) {
    for (uint32_t lba = ISO9660_PVD_LBA; lba < ISO9660_PVD_LBA + 16; lba++) {
        if (dev->read_sectors(dev, (uint64_t)lba * sectors_per_block, sectors_per_block, out_2048) != 0)
            return FS_ERR_IO;
        if (memcmp(out_2048 + 1, ISO9660_ID, 5) != 0) return FS_ERR_CORRUPT;
        uint8_t type = out_2048[0];
        if (type == ISO9660_VD_TYPE_PRIMARY) return FS_OK;
        if (type == ISO9660_VD_TYPE_TERMINATOR) return FS_ERR_NOENT;
    }
    return FS_ERR_NOENT;
}

int iso9660_probe(struct block_device *dev) {
    if (!dev) return FS_ERR_PARAM;
    uint32_t sector_size = dev->sector_size ? dev->sector_size : 512;
    if (ISO9660_BLOCK_SIZE % sector_size != 0) return FS_ERR_NOSUPP;
    uint32_t sectors_per_block = ISO9660_BLOCK_SIZE / sector_size;

    uint8_t *buf = kmalloc(ISO9660_BLOCK_SIZE);
    if (!buf) return FS_ERR_NOMEM;
    int r = find_pvd(dev, sectors_per_block, buf);
    kfree(buf);
    return r;
}

int iso9660_mount(struct block_device *dev, fs_t *out) {
    if (!dev || !out) return FS_ERR_PARAM;
    uint32_t sector_size = dev->sector_size ? dev->sector_size : 512;
    if (ISO9660_BLOCK_SIZE % sector_size != 0) return FS_ERR_NOSUPP;
    uint32_t sectors_per_block = ISO9660_BLOCK_SIZE / sector_size;

    uint8_t *pvd = kmalloc(ISO9660_BLOCK_SIZE);
    if (!pvd) return FS_ERR_NOMEM;
    int r = find_pvd(dev, sectors_per_block, pvd);
    if (r != FS_OK) { kfree(pvd); return r; }

    iso9660_fs_t *fs = kmalloc(sizeof(iso9660_fs_t));
    if (!fs) { kfree(pvd); return FS_ERR_NOMEM; }
    memset(fs, 0, sizeof(*fs));

    fs->dev = dev;
    fs->sectors_per_block = sectors_per_block;

    iso9660_dirrecord_t root;
    memcpy(&root, pvd + 156, sizeof(root));
    fs->root_extent_lba = root.extent_lba_le;
    fs->root_data_length = root.data_len_le;

    memcpy(fs->label, pvd + 40, 32);
    fs->label[32] = '\0';
    for (int i = 31; i >= 0 && fs->label[i] == ' '; i--) fs->label[i] = '\0';

    kfree(pvd);

    out->type = FS_TYPE_ISO9660;
    out->dev = dev;
    out->ops = &iso9660_ops;
    out->priv = fs;
    strncpy(out->label, fs->label, sizeof(out->label) - 1);
    out->label[sizeof(out->label) - 1] = '\0';

    ILOG("mounted volume '%s' (read-only), root_extent=%u, size=%u bytes\n",
         fs->label[0] ? fs->label : "(no label)",
         (unsigned)fs->root_extent_lba, (unsigned)fs->root_data_length);

    return FS_OK;
}
