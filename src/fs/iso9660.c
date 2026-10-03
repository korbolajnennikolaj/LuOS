#include "fs/iso9660.h"

#include "components/logger.h"
#include "components/Memory/heap.h"

#include <string.h>

static int read_iso_block(iso9660_fs_t *fs, uint32_t iso_lba, void *buf) {
    uint64_t dev_lba = (uint64_t)iso_lba * fs->sectors_per_block;
    if (fs->dev->read_sectors(fs->dev, dev_lba, fs->sectors_per_block, buf) != 0) {
        LOG_ERROR("%s: failed to read ISO block %u", fs->dev->name, iso_lba);
        return FS_ERR_IO;
    }
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

static size_t utf8_put(char *out, size_t pos, size_t cap, uint32_t cp) {
    char tmp[4];
    size_t n;
    if (cp < 0x80) { tmp[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { tmp[0] = (char)(0xC0 | (cp >> 6)); tmp[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12)); tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        tmp[0] = (char)(0xF0 | (cp >> 18)); tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    }
    if (pos + n >= cap) return pos;
    memcpy(out + pos, tmp, n);
    return pos + n;
}

static void joliet_name(const uint8_t *raw, uint8_t raw_len, char *out, size_t out_cap) {
    size_t pos = 0;
    for (int i = 0; i + 1 < raw_len; i += 2) {
        uint32_t cp = ((uint32_t)raw[i] << 8) | raw[i + 1];
        if (cp == ';') break;
        if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < raw_len) {
            uint32_t lo = ((uint32_t)raw[i + 2] << 8) | raw[i + 3];
            if (lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        pos = utf8_put(out, pos, out_cap, cp);
    }
    if (pos > 1 && out[pos - 1] == '.') pos--;
    out[pos] = '\0';
}

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void rr_scan(iso9660_fs_t *fs, const uint8_t *recp, char *name, size_t name_cap,
                    bool *has_name, uint32_t *child_lba, bool *relocated) {
    uint8_t name_len = recp[32];
    uint32_t start = 33u + name_len + ((name_len & 1) ? 0u : 1u) + fs->susp_skip;
    const uint8_t *area = recp + start;
    uint32_t len = recp[0] > start ? recp[0] - start : 0;
    uint8_t *ce_buf = NULL;
    size_t pos = 0;
    bool name_done = false;

    for (int hops = 0; hops < 8 && len; hops++) {
        uint32_t ce_lba = 0, ce_off = 0, ce_len = 0;
        uint32_t o = 0;
        while (o + 4 <= len) {
            const uint8_t *e = area + o;
            uint8_t el = e[2];
            if (el < 4 || o + el > len) break;
            if (e[0] == 'S' && e[1] == 'T') break;
            if (e[0] == 'N' && e[1] == 'M' && el >= 5 && !name_done) {
                uint8_t fl = e[4];
                if (!(fl & 0x06)) {
                    for (uint32_t k = 5; k < el && pos + 1 < name_cap; k++) name[pos++] = (char)e[k];
                    *has_name = true;
                }
                if (!(fl & 0x01)) name_done = true;
            } else if (e[0] == 'C' && e[1] == 'E' && el >= 28) {
                ce_lba = rd32(e + 4);
                ce_off = rd32(e + 12);
                ce_len = rd32(e + 20);
            } else if (e[0] == 'C' && e[1] == 'L' && el >= 12) {
                *child_lba = rd32(e + 4);
            } else if (e[0] == 'R' && e[1] == 'E') {
                *relocated = true;
            }
            o += el;
        }
        if (!ce_len || ce_off >= ISO9660_BLOCK_SIZE) break;
        if (!ce_buf) ce_buf = kmalloc(ISO9660_BLOCK_SIZE);
        if (!ce_buf || read_iso_block(fs, ce_lba, ce_buf) != FS_OK) break;
        if (ce_off + ce_len > ISO9660_BLOCK_SIZE) ce_len = ISO9660_BLOCK_SIZE - ce_off;
        area = ce_buf + ce_off;
        len = ce_len;
    }

    if (ce_buf) kfree(ce_buf);
    name[pos] = '\0';
}

typedef struct {
    uint32_t extent_lba;
    uint32_t data_length;
    bool is_dir;
    char name_utf8[FS_MAX_NAME + 1];
} iso9660_entry_info_t;

static int decode_record(iso9660_fs_t *fs, const uint8_t *recp, iso9660_entry_info_t *out) {
    iso9660_dirrecord_t rec;
    memcpy(&rec, recp, sizeof(rec));
    const char *ident = (const char *)(recp + sizeof(iso9660_dirrecord_t));
    if (rec.name_len == 1 && (ident[0] == 0x00 || ident[0] == 0x01)) return 0;

    out->extent_lba = rec.extent_lba_le;
    out->data_length = rec.data_len_le;
    out->is_dir = (rec.file_flags & ISO9660_FLAG_DIRECTORY) != 0;

    if (fs->name_mode == ISO9660_NAMES_JOLIET) {
        joliet_name((const uint8_t *)ident, rec.name_len, out->name_utf8, sizeof(out->name_utf8));
        return out->name_utf8[0] ? 1 : 0;
    }

    if (fs->name_mode == ISO9660_NAMES_ROCKRIDGE) {
        bool has_name = false, relocated = false;
        uint32_t child = 0;
        rr_scan(fs, recp, out->name_utf8, sizeof(out->name_utf8), &has_name, &child, &relocated);
        if (relocated) return 0;
        if (child) {
            uint8_t *blk = kmalloc(ISO9660_BLOCK_SIZE);
            if (!blk) return FS_ERR_NOMEM;
            if (read_iso_block(fs, child, blk) != FS_OK) { kfree(blk); return FS_ERR_IO; }
            iso9660_dirrecord_t dot;
            memcpy(&dot, blk, sizeof(dot));
            out->extent_lba = dot.extent_lba_le;
            out->data_length = dot.data_len_le;
            out->is_dir = true;
            kfree(blk);
        }
        if (has_name && out->name_utf8[0]) return 1;
    }

    clean_iso_name(ident, rec.name_len, out->name_utf8, sizeof(out->name_utf8));
    return 1;
}

static bool names_equal(iso9660_fs_t *fs, const char *a, const char *b) {
    if (fs->name_mode == ISO9660_NAMES_ROCKRIDGE) return strcmp(a, b) == 0;
    return ascii_ieq(a, b);
}

static int dir_find(iso9660_fs_t *fs, uint32_t dir_extent_lba, uint32_t dir_data_length,
                     const char *name, iso9660_entry_info_t *out) {
    uint8_t *buf; uint32_t buf_len;
    int r = read_whole_dir(fs, dir_extent_lba, dir_data_length, &buf, &buf_len);
    if (r != FS_OK) return r;

    int ret = FS_ERR_NOENT;
    uint32_t off = 0;
    while (off < dir_data_length && off < buf_len) {
        uint8_t reclen = buf[off];
        if (reclen == 0 || off + reclen > buf_len) {
            off = (off + ISO9660_BLOCK_SIZE) & ~(ISO9660_BLOCK_SIZE - 1);
            continue;
        }

        iso9660_entry_info_t info;
        int d = decode_record(fs, buf + off, &info);
        if (d < 0) { ret = d; break; }
        if (d > 0 && names_equal(fs, info.name_utf8, name)) {
            *out = info;
            ret = FS_OK;
            break;
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

    while (it->offset < it->data_len && it->offset < it->buf_len) {
        uint8_t reclen = it->buf[it->offset];
        if (reclen == 0 || it->offset + reclen > it->buf_len) {
            it->offset = (it->offset + ISO9660_BLOCK_SIZE) & ~(ISO9660_BLOCK_SIZE - 1);
            continue;
        }

        iso9660_entry_info_t info;
        int d = decode_record(it->fs, it->buf + it->offset, &info);
        it->offset += reclen;
        if (d < 0) return d;
        if (d == 0) continue;

        strncpy(out->name, info.name_utf8, FS_MAX_NAME); out->name[FS_MAX_NAME] = '\0';
        out->type = info.is_dir ? FS_ENTRY_DIR : FS_ENTRY_FILE;
        out->size = info.data_length;
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

    if (read_iso_block(fs, fs->root_extent_lba, pvd) == FS_OK && pvd[0] >= 41 && pvd[32] == 1 &&
        pvd[34] == 'S' && pvd[35] == 'P' && pvd[36] >= 7 && pvd[38] == 0xBE && pvd[39] == 0xEF) {
        fs->name_mode = ISO9660_NAMES_ROCKRIDGE;
        fs->susp_skip = pvd[40];
    }

    if (fs->name_mode == ISO9660_NAMES_PLAIN) {
        for (uint32_t lba = ISO9660_PVD_LBA + 1; lba < ISO9660_PVD_LBA + 32; lba++) {
            if (read_iso_block(fs, lba, pvd) != FS_OK) break;
            if (memcmp(pvd + 1, ISO9660_ID, 5) != 0 || pvd[0] == ISO9660_VD_TYPE_TERMINATOR) break;
            if (pvd[0] == ISO9660_VD_TYPE_SUPPLEMENTARY && pvd[88] == '%' && pvd[89] == '/' &&
                (pvd[90] == '@' || pvd[90] == 'C' || pvd[90] == 'E')) {
                memcpy(&root, pvd + 156, sizeof(root));
                fs->root_extent_lba = root.extent_lba_le;
                fs->root_data_length = root.data_len_le;
                fs->name_mode = ISO9660_NAMES_JOLIET;
                break;
            }
        }
    }

    kfree(pvd);

    out->type = FS_TYPE_ISO9660;
    out->dev = dev;
    out->ops = &iso9660_ops;
    out->priv = fs;
    strncpy(out->label, fs->label, sizeof(out->label) - 1);
    out->label[sizeof(out->label) - 1] = '\0';

    LOG_DEBUG("mounted volume '%s' (read-only), root_extent=%u, size=%u bytes, names=%s",
         fs->label[0] ? fs->label : "(no label)",
         (unsigned)fs->root_extent_lba, (unsigned)fs->root_data_length,
         fs->name_mode == ISO9660_NAMES_ROCKRIDGE ? "rock ridge" :
         fs->name_mode == ISO9660_NAMES_JOLIET ? "joliet" : "iso9660");

    return FS_OK;
}
