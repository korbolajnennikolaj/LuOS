#include "fs/archive.h"

#include "components/logger.h"
#include "components/Memory/heap.h"

#include <string.h>

#define ARCHIVE_BOUNCE_SIZE 65536u

static int dev_read(archive_fs_t *fs, uint64_t offset, void *buf, uint64_t len) {
    uint32_t ss = fs->dev->sector_size ? fs->dev->sector_size : 512;
    uint64_t dev_bytes = fs->dev->sector_count * ss;
    if (offset > dev_bytes || len > dev_bytes - offset) return FS_ERR_IO;

    uint8_t *out = (uint8_t *)buf;
    while (len > 0) {
        uint64_t lba = offset / ss;
        uint32_t skip = (uint32_t)(offset % ss);
        uint32_t sectors = (uint32_t)((skip + len + ss - 1) / ss);
        if (sectors > ARCHIVE_BOUNCE_SIZE / ss) sectors = ARCHIVE_BOUNCE_SIZE / ss;
        if (fs->dev->read_sectors(fs->dev, lba, sectors, fs->bounce) != 0) return FS_ERR_IO;
        uint64_t got = (uint64_t)sectors * ss - skip;
        if (got > len) got = len;
        memcpy(out, fs->bounce + skip, (size_t)got);
        out += got;
        offset += got;
        len -= got;
    }
    return FS_OK;
}

static uint64_t parse_octal(const uint8_t *s, int n) {
    if (s[0] & 0x80) {
        uint64_t v = s[0] & 0x7F;
        for (int i = 1; i < n; i++) v = (v << 8) | s[i];
        return v;
    }
    uint64_t v = 0;
    int i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\0')) i++;
    for (; i < n && s[i] >= '0' && s[i] <= '7'; i++) v = v * 8 + (uint64_t)(s[i] - '0');
    return v;
}

static bool parse_hex8(const uint8_t *s, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t c = s[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

static bool tar_checksum_ok(const uint8_t *h) {
    uint64_t want = parse_octal(h + 148, 8);
    uint64_t sum = 0;
    for (int i = 0; i < (int)TAR_BLOCK_SIZE; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
    return sum == want;
}

static archive_node_t *node_new(const char *name, size_t len, bool is_dir) {
    archive_node_t *n = kmalloc(sizeof(archive_node_t));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    if (len > FS_MAX_NAME) len = FS_MAX_NAME;
    memcpy(n->name, name, len);
    n->name[len] = '\0';
    n->is_dir = is_dir;
    return n;
}

static archive_node_t *node_child(archive_node_t *dir, const char *name, size_t len) {
    for (archive_node_t *c = dir->first_child; c; c = c->next_sibling)
        if (strlen(c->name) == len && memcmp(c->name, name, len) == 0) return c;
    return NULL;
}

static void node_link(archive_node_t *dir, archive_node_t *n) {
    n->parent = dir;
    if (!dir->first_child) { dir->first_child = n; return; }
    archive_node_t *c = dir->first_child;
    while (c->next_sibling) c = c->next_sibling;
    c->next_sibling = n;
}

static void node_free_tree(archive_node_t *n) {
    archive_node_t *c = n->first_child;
    while (c) {
        archive_node_t *next = c->next_sibling;
        node_free_tree(c);
        c = next;
    }
    kfree(n);
}

static int add_entry(archive_fs_t *fs, const char *path, bool is_dir, uint64_t offset, uint64_t size) {
    archive_node_t *cur = fs->root;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);
        while (*p == '/') p++;
        bool last = (*p == '\0');

        if (len == 1 && start[0] == '.') continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') return FS_ERR_PARAM;

        archive_node_t *child = node_child(cur, start, len);
        if (!child) {
            child = node_new(start, len, last ? is_dir : true);
            if (!child) return FS_ERR_NOMEM;
            node_link(cur, child);
            if (child->is_dir) fs->dir_count++;
            else fs->file_count++;
        }
        if (last) {
            if (!is_dir) {
                if (child->is_dir) { fs->dir_count--; fs->file_count++; }
                child->is_dir = false;
                child->offset = offset;
                child->size = size;
            }
            return FS_OK;
        }
        if (!child->is_dir) return FS_ERR_CORRUPT;
        cur = child;
    }
    return FS_OK;
}

static int scan_tar(archive_fs_t *fs) {
    uint32_t ss = fs->dev->sector_size ? fs->dev->sector_size : 512;
    uint64_t end = fs->dev->sector_count * ss;
    uint8_t h[TAR_BLOCK_SIZE];
    char *pending = kmalloc(FS_MAX_PATH);
    char *path = kmalloc(FS_MAX_PATH);
    if (!pending || !path) {
        if (pending) kfree(pending);
        if (path) kfree(path);
        return FS_ERR_NOMEM;
    }
    pending[0] = '\0';

    int r = FS_OK;
    uint64_t off = 0;
    while (off + TAR_BLOCK_SIZE <= end) {
        r = dev_read(fs, off, h, TAR_BLOCK_SIZE);
        if (r != FS_OK) break;

        bool zero = true;
        for (uint32_t i = 0; i < TAR_BLOCK_SIZE && zero; i++) if (h[i]) zero = false;
        if (zero) break;
        if (!tar_checksum_ok(h)) {
            LOG_WARNING("%s: bad tar header checksum at offset %llu", fs->dev->name, (unsigned long long)off);
            break;
        }

        uint64_t size = parse_octal(h + 124, 12);
        uint8_t type = h[156];
        uint64_t data = off + TAR_BLOCK_SIZE;
        uint64_t next = data + ((size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE) * TAR_BLOCK_SIZE;
        if (next > end) { r = FS_ERR_CORRUPT; break; }

        if (type == 'L' || type == 'x') {
            uint64_t n = size < FS_MAX_PATH ? size : FS_MAX_PATH - 1;
            char *tmp = kmalloc((size_t)size + 1);
            if (!tmp) { r = FS_ERR_NOMEM; break; }
            r = dev_read(fs, data, tmp, size);
            if (r != FS_OK) { kfree(tmp); break; }
            tmp[size] = '\0';
            if (type == 'L') {
                memcpy(pending, tmp, (size_t)n);
                pending[n] = '\0';
            } else {
                char *q = tmp;
                while (q < tmp + size) {
                    char *rec_end = q;
                    while (rec_end < tmp + size && *rec_end != '\n') rec_end++;
                    char *sp = q;
                    while (sp < rec_end && *sp != ' ') sp++;
                    if (sp + 6 <= rec_end && memcmp(sp + 1, "path=", 5) == 0) {
                        size_t len = (size_t)(rec_end - (sp + 6));
                        if (len >= FS_MAX_PATH) len = FS_MAX_PATH - 1;
                        memcpy(pending, sp + 6, len);
                        pending[len] = '\0';
                    }
                    q = rec_end + 1;
                }
            }
            kfree(tmp);
            off = next;
            continue;
        }

        if (pending[0]) {
            strcpy(path, pending);
            pending[0] = '\0';
        } else {
            size_t plen = 0;
            if (memcmp(h + TAR_MAGIC_OFFSET, "ustar", 5) == 0 && h[345]) {
                while (plen < 155 && h[345 + plen]) { path[plen] = (char)h[345 + plen]; plen++; }
                path[plen++] = '/';
            }
            size_t nlen = 0;
            while (nlen < 100 && h[nlen]) { path[plen + nlen] = (char)h[nlen]; nlen++; }
            path[plen + nlen] = '\0';
        }

        if (type == '5') r = add_entry(fs, path, true, 0, 0);
        else if (type == '0' || type == '\0' || type == '7') r = add_entry(fs, path, false, data, size);
        if (r == FS_ERR_PARAM) r = FS_OK;
        if (r != FS_OK) break;
        off = next;
    }

    kfree(pending);
    kfree(path);
    return r;
}

static int scan_cpio(archive_fs_t *fs) {
    uint32_t ss = fs->dev->sector_size ? fs->dev->sector_size : 512;
    uint64_t end = fs->dev->sector_count * ss;
    uint8_t h[CPIO_NEWC_HEADER_SIZE];
    char *name = kmalloc(FS_MAX_PATH);
    if (!name) return FS_ERR_NOMEM;

    int r = FS_OK;
    uint64_t off = 0;
    while (off + CPIO_NEWC_HEADER_SIZE <= end) {
        r = dev_read(fs, off, h, CPIO_NEWC_HEADER_SIZE);
        if (r != FS_OK) break;
        if (memcmp(h, "07070", 5) != 0 || (h[5] != '1' && h[5] != '2')) { r = FS_ERR_CORRUPT; break; }

        uint32_t mode, filesize, namesize;
        if (!parse_hex8(h + 14, &mode) || !parse_hex8(h + 54, &filesize) || !parse_hex8(h + 94, &namesize)) {
            r = FS_ERR_CORRUPT;
            break;
        }
        if (namesize == 0 || namesize >= FS_MAX_PATH) { r = FS_ERR_CORRUPT; break; }

        r = dev_read(fs, off + CPIO_NEWC_HEADER_SIZE, name, namesize);
        if (r != FS_OK) break;
        name[namesize - 1] = '\0';

        uint64_t data = (off + CPIO_NEWC_HEADER_SIZE + namesize + 3) & ~3ull;
        uint64_t next = (data + filesize + 3) & ~3ull;
        if (strcmp(name, "TRAILER!!!") == 0) break;
        if (data + filesize > end) { r = FS_ERR_CORRUPT; break; }

        uint32_t kind = mode & 0170000u;
        if (kind == 0040000u) r = add_entry(fs, name, true, 0, 0);
        else if (kind == 0100000u) r = add_entry(fs, name, false, data, filesize);
        if (r == FS_ERR_PARAM) r = FS_OK;
        if (r != FS_OK) break;
        off = next;
    }

    kfree(name);
    return r;
}

static enum archive_format detect(struct block_device *dev, uint8_t *sector) {
    if (dev->read_sectors(dev, 0, 1, sector) != 0) return ARCHIVE_FORMAT_NONE;
    if (memcmp(sector, "07070", 5) == 0 && (sector[5] == '1' || sector[5] == '2')) return ARCHIVE_FORMAT_CPIO;
    if ((dev->sector_size ? dev->sector_size : 512) >= TAR_BLOCK_SIZE &&
        memcmp(sector + TAR_MAGIC_OFFSET, "ustar", 5) == 0 && tar_checksum_ok(sector))
        return ARCHIVE_FORMAT_TAR;
    return ARCHIVE_FORMAT_NONE;
}

static int resolve(archive_fs_t *fs, const char *path, archive_node_t **out) {
    fs_path_parts_t pp;
    int r = fs_split_path(path, &pp);
    if (r != FS_OK) return r;
    archive_node_t *cur = fs->root;
    for (int i = 0; i < pp.count; i++) {
        if (!cur->is_dir) return FS_ERR_NOTDIR;
        cur = node_child(cur, pp.comps[i], strlen(pp.comps[i]));
        if (!cur) return FS_ERR_NOENT;
    }
    *out = cur;
    return FS_OK;
}

static int archive_op_open(fs_t *fsroot, const char *path, bool create, bool truncate_flag, fs_file_t *out) {
    archive_fs_t *fs = (archive_fs_t *)fsroot->priv;
    archive_node_t *n;
    int r = resolve(fs, path, &n);
    if (r == FS_ERR_NOENT && create) return FS_ERR_NOSUPP;
    if (r != FS_OK) return r;
    if (n->is_dir) return FS_ERR_ISDIR;
    if (truncate_flag) return FS_ERR_NOSUPP;

    archive_file_t *af = kmalloc(sizeof(archive_file_t));
    if (!af) return FS_ERR_NOMEM;
    af->fs = fs;
    af->node = n;
    out->priv = af;
    out->size = n->size;
    out->pos = 0;
    out->writable = false;
    return FS_OK;
}

static int archive_op_close(fs_file_t *f) {
    if (f->priv) kfree(f->priv);
    f->priv = NULL;
    return FS_OK;
}

static int64_t archive_op_read(fs_file_t *f, void *buf, uint64_t size) {
    archive_file_t *af = (archive_file_t *)f->priv;
    if (f->pos >= af->node->size) return 0;
    if (size > af->node->size - f->pos) size = af->node->size - f->pos;
    int r = dev_read(af->fs, af->node->offset + f->pos, buf, size);
    if (r != FS_OK) return r;
    f->pos += size;
    return (int64_t)size;
}

static int64_t archive_op_write(fs_file_t *f, const void *buf, uint64_t size) {
    (void)f; (void)buf; (void)size;
    return FS_ERR_NOSUPP;
}

static int archive_op_seek(fs_file_t *f, uint64_t pos) { f->pos = pos; return FS_OK; }

static int archive_op_truncate(fs_file_t *f, uint64_t size) { (void)f; (void)size; return FS_ERR_NOSUPP; }

static int archive_op_opendir(fs_t *fsroot, const char *path, fs_dir_t *out) {
    archive_fs_t *fs = (archive_fs_t *)fsroot->priv;
    archive_node_t *n;
    int r = resolve(fs, path, &n);
    if (r != FS_OK) return r;
    if (!n->is_dir) return FS_ERR_NOTDIR;
    archive_diriter_t *it = kmalloc(sizeof(archive_diriter_t));
    if (!it) return FS_ERR_NOMEM;
    it->next = n->first_child;
    out->priv = it;
    return FS_OK;
}

static int archive_op_readdir(fs_dir_t *dir, fs_dirent_t *out) {
    archive_diriter_t *it = (archive_diriter_t *)dir->priv;
    if (!it->next) return FS_ERR_EOF;
    archive_node_t *n = it->next;
    it->next = n->next_sibling;
    strncpy(out->name, n->name, FS_MAX_NAME);
    out->name[FS_MAX_NAME] = '\0';
    out->type = n->is_dir ? FS_ENTRY_DIR : FS_ENTRY_FILE;
    out->size = n->size;
    return FS_OK;
}

static int archive_op_closedir(fs_dir_t *dir) {
    if (dir->priv) kfree(dir->priv);
    dir->priv = NULL;
    return FS_OK;
}

static int archive_op_stat(fs_t *fsroot, const char *path, fs_dirent_t *out) {
    archive_fs_t *fs = (archive_fs_t *)fsroot->priv;
    archive_node_t *n;
    int r = resolve(fs, path, &n);
    if (r != FS_OK) return r;
    strncpy(out->name, n == fs->root ? "/" : n->name, FS_MAX_NAME);
    out->name[FS_MAX_NAME] = '\0';
    out->type = n->is_dir ? FS_ENTRY_DIR : FS_ENTRY_FILE;
    out->size = n->size;
    return FS_OK;
}

static int archive_op_readonly_path(fs_t *fsroot, const char *path) {
    (void)fsroot; (void)path;
    return FS_ERR_NOSUPP;
}

static int archive_op_unmount(fs_t *fsroot) {
    archive_fs_t *fs = (archive_fs_t *)fsroot->priv;
    if (fs) {
        if (fs->root) node_free_tree(fs->root);
        if (fs->bounce) kfree(fs->bounce);
        kfree(fs);
    }
    fsroot->priv = NULL;
    return FS_OK;
}

const fs_ops_t archive_ops = {
    .open = archive_op_open,
    .close = archive_op_close,
    .read = archive_op_read,
    .write = archive_op_write,
    .seek = archive_op_seek,
    .truncate = archive_op_truncate,
    .opendir = archive_op_opendir,
    .readdir = archive_op_readdir,
    .closedir = archive_op_closedir,
    .mkdir = archive_op_readonly_path,
    .unlink = archive_op_readonly_path,
    .rmdir = archive_op_readonly_path,
    .stat = archive_op_stat,
    .unmount = archive_op_unmount,
};

int archive_probe(struct block_device *dev) {
    if (!dev) return FS_ERR_PARAM;
    uint32_t ss = dev->sector_size ? dev->sector_size : 512;
    uint8_t *buf = kmalloc(ss);
    if (!buf) return FS_ERR_NOMEM;
    enum archive_format f = detect(dev, buf);
    kfree(buf);
    return f == ARCHIVE_FORMAT_NONE ? FS_ERR_CORRUPT : FS_OK;
}

int archive_mount(struct block_device *dev, fs_t *out) {
    if (!dev || !out) return FS_ERR_PARAM;

    archive_fs_t *fs = kmalloc(sizeof(archive_fs_t));
    if (!fs) return FS_ERR_NOMEM;
    memset(fs, 0, sizeof(*fs));
    fs->dev = dev;
    fs->bounce = kmalloc(ARCHIVE_BOUNCE_SIZE);
    fs->root = node_new("", 0, true);
    if (!fs->bounce || !fs->root) {
        if (fs->bounce) kfree(fs->bounce);
        if (fs->root) kfree(fs->root);
        kfree(fs);
        return FS_ERR_NOMEM;
    }

    fs->format = detect(dev, fs->bounce);
    int r = FS_ERR_CORRUPT;
    if (fs->format == ARCHIVE_FORMAT_TAR) r = scan_tar(fs);
    else if (fs->format == ARCHIVE_FORMAT_CPIO) r = scan_cpio(fs);

    if (r != FS_OK) {
        LOG_ERROR("%s: archive scan failed (%d)", dev->name, r);
        node_free_tree(fs->root);
        kfree(fs->bounce);
        kfree(fs);
        return r;
    }

    out->type = FS_TYPE_ARCHIVE;
    out->dev = dev;
    out->ops = &archive_ops;
    out->priv = fs;
    strncpy(out->label, fs->format == ARCHIVE_FORMAT_TAR ? "tar" : "cpio", sizeof(out->label) - 1);
    out->label[sizeof(out->label) - 1] = '\0';

    LOG_DEBUG("mounted %s archive on '%s': %u files, %u dirs, read-only",
              fs->format == ARCHIVE_FORMAT_TAR ? "tar" : "cpio", dev->name,
              (unsigned)fs->file_count, (unsigned)fs->dir_count);
    return FS_OK;
}
