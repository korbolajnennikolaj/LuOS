#include "fs/ramfs.h"

#include "components/logger.h"
#include "components/Memory/heap.h"

#include <string.h>

#define RAMFS_MIN_CAPACITY 256u

static ramfs_node_t *node_new(const char *name, bool is_dir) {
    ramfs_node_t *n = kmalloc(sizeof(ramfs_node_t));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    strncpy(n->name, name, FS_MAX_NAME);
    n->name[FS_MAX_NAME] = '\0';
    n->is_dir = is_dir;
    return n;
}

static void node_free(ramfs_fs_t *fs, ramfs_node_t *n) {
    if (n->data) {
        fs->bytes_used -= n->capacity;
        kfree(n->data);
    }
    if (fs->node_count) fs->node_count--;
    kfree(n);
}

static void node_free_tree(ramfs_fs_t *fs, ramfs_node_t *n) {
    ramfs_node_t *c = n->first_child;
    while (c) {
        ramfs_node_t *next = c->next_sibling;
        node_free_tree(fs, c);
        c = next;
    }
    node_free(fs, n);
}

static ramfs_node_t *dir_lookup(ramfs_node_t *dir, const char *name) {
    for (ramfs_node_t *c = dir->first_child; c; c = c->next_sibling)
        if (strcmp(c->name, name) == 0) return c;
    return NULL;
}

static void dir_link(ramfs_node_t *dir, ramfs_node_t *n) {
    n->parent = dir;
    n->next_sibling = NULL;
    if (!dir->first_child) { dir->first_child = n; return; }
    ramfs_node_t *c = dir->first_child;
    while (c->next_sibling) c = c->next_sibling;
    c->next_sibling = n;
}

static void dir_unlink(ramfs_node_t *dir, ramfs_node_t *n) {
    ramfs_node_t **pp = &dir->first_child;
    while (*pp) {
        if (*pp == n) { *pp = n->next_sibling; break; }
        pp = &(*pp)->next_sibling;
    }
    n->parent = NULL;
    n->next_sibling = NULL;
}

static int resolve(ramfs_fs_t *fs, const char *path, ramfs_node_t **out) {
    fs_path_parts_t pp;
    int r = fs_split_path(path, &pp);
    if (r != FS_OK) return r;

    ramfs_node_t *cur = fs->root;
    for (int i = 0; i < pp.count; i++) {
        if (!cur->is_dir) return FS_ERR_NOTDIR;
        cur = dir_lookup(cur, pp.comps[i]);
        if (!cur) return FS_ERR_NOENT;
    }
    *out = cur;
    return FS_OK;
}

static int resolve_parent(ramfs_fs_t *fs, const char *path, ramfs_node_t **parent, char *leaf) {
    fs_path_parts_t pp;
    int r = fs_split_path(path, &pp);
    if (r != FS_OK) return r;
    if (pp.count == 0) return FS_ERR_PARAM;

    ramfs_node_t *cur = fs->root;
    for (int i = 0; i < pp.count - 1; i++) {
        cur = dir_lookup(cur, pp.comps[i]);
        if (!cur) return FS_ERR_NOENT;
        if (!cur->is_dir) return FS_ERR_NOTDIR;
    }

    const char *name = pp.comps[pp.count - 1];
    if (strlen(name) > FS_MAX_NAME) return FS_ERR_PARAM;
    strcpy(leaf, name);
    *parent = cur;
    return FS_OK;
}

static int node_reserve(ramfs_fs_t *fs, ramfs_node_t *n, uint64_t size) {
    if (size <= n->capacity) return FS_OK;
    uint64_t cap = n->capacity ? n->capacity : RAMFS_MIN_CAPACITY;
    while (cap < size) cap *= 2;
    uint8_t *p = krealloc(n->data, (size_t)cap);
    if (!p) return FS_ERR_NOSPACE;
    memset(p + n->capacity, 0, (size_t)(cap - n->capacity));
    fs->bytes_used += cap - n->capacity;
    n->data = p;
    n->capacity = cap;
    return FS_OK;
}

static int node_resize(ramfs_fs_t *fs, ramfs_node_t *n, uint64_t size) {
    if (size > n->size) {
        int r = node_reserve(fs, n, size);
        if (r != FS_OK) return r;
        memset(n->data + n->size, 0, (size_t)(size - n->size));
    } else if (size < n->size) {
        memset(n->data + size, 0, (size_t)(n->size - size));
        if (size == 0 && n->data) {
            fs->bytes_used -= n->capacity;
            kfree(n->data);
            n->data = NULL;
            n->capacity = 0;
        }
    }
    n->size = size;
    return FS_OK;
}

static int ramfs_op_open(fs_t *fsroot, const char *path, bool create, bool truncate_flag, fs_file_t *out) {
    ramfs_fs_t *fs = (ramfs_fs_t *)fsroot->priv;

    ramfs_node_t *parent;
    char leaf[FS_MAX_NAME + 1];
    int r = resolve_parent(fs, path, &parent, leaf);
    if (r != FS_OK) return r;

    ramfs_node_t *n = dir_lookup(parent, leaf);
    if (n) {
        if (n->is_dir) return FS_ERR_ISDIR;
        if (truncate_flag) node_resize(fs, n, 0);
    } else {
        if (!create) return FS_ERR_NOENT;
        n = node_new(leaf, false);
        if (!n) return FS_ERR_NOMEM;
        dir_link(parent, n);
        fs->node_count++;
    }

    ramfs_file_t *rf = kmalloc(sizeof(ramfs_file_t));
    if (!rf) return FS_ERR_NOMEM;
    rf->fs = fs;
    rf->node = n;
    n->open_count++;

    out->priv = rf;
    out->size = n->size;
    out->pos = 0;
    out->writable = true;
    return FS_OK;
}

static int ramfs_op_close(fs_file_t *f) {
    ramfs_file_t *rf = (ramfs_file_t *)f->priv;
    if (!rf) return FS_OK;
    ramfs_node_t *n = rf->node;
    if (n->open_count) n->open_count--;
    if (n->unlinked && n->open_count == 0) node_free(rf->fs, n);
    kfree(rf);
    f->priv = NULL;
    return FS_OK;
}

static int64_t ramfs_op_read(fs_file_t *f, void *buf, uint64_t size) {
    ramfs_file_t *rf = (ramfs_file_t *)f->priv;
    ramfs_node_t *n = rf->node;
    if (f->pos >= n->size) return 0;
    uint64_t remaining = n->size - f->pos;
    if (size > remaining) size = remaining;
    memcpy(buf, n->data + f->pos, (size_t)size);
    f->pos += size;
    return (int64_t)size;
}

static int64_t ramfs_op_write(fs_file_t *f, const void *buf, uint64_t size) {
    ramfs_file_t *rf = (ramfs_file_t *)f->priv;
    ramfs_node_t *n = rf->node;
    if (size == 0) return 0;

    uint64_t end = f->pos + size;
    if (end > n->size) {
        int r = node_resize(rf->fs, n, end);
        if (r != FS_OK) return r;
    }
    memcpy(n->data + f->pos, buf, (size_t)size);
    f->pos = end;
    f->size = n->size;
    return (int64_t)size;
}

static int ramfs_op_seek(fs_file_t *f, uint64_t pos) {
    f->pos = pos;
    return FS_OK;
}

static int ramfs_op_truncate(fs_file_t *f, uint64_t size) {
    ramfs_file_t *rf = (ramfs_file_t *)f->priv;
    int r = node_resize(rf->fs, rf->node, size);
    if (r != FS_OK) return r;
    f->size = size;
    if (f->pos > size) f->pos = size;
    return FS_OK;
}

static int ramfs_op_opendir(fs_t *fsroot, const char *path, fs_dir_t *out) {
    ramfs_fs_t *fs = (ramfs_fs_t *)fsroot->priv;
    ramfs_node_t *n;
    int r = resolve(fs, path, &n);
    if (r != FS_OK) return r;
    if (!n->is_dir) return FS_ERR_NOTDIR;

    ramfs_diriter_t *it = kmalloc(sizeof(ramfs_diriter_t));
    if (!it) return FS_ERR_NOMEM;
    it->dir = n;
    it->index = 0;
    out->priv = it;
    return FS_OK;
}

static int ramfs_op_readdir(fs_dir_t *dir, fs_dirent_t *out) {
    ramfs_diriter_t *it = (ramfs_diriter_t *)dir->priv;
    ramfs_node_t *c = it->dir->first_child;
    for (uint32_t i = 0; c && i < it->index; i++) c = c->next_sibling;
    if (!c) return FS_ERR_EOF;
    it->index++;

    strncpy(out->name, c->name, FS_MAX_NAME);
    out->name[FS_MAX_NAME] = '\0';
    out->type = c->is_dir ? FS_ENTRY_DIR : FS_ENTRY_FILE;
    out->size = c->size;
    return FS_OK;
}

static int ramfs_op_closedir(fs_dir_t *dir) {
    if (dir->priv) kfree(dir->priv);
    dir->priv = NULL;
    return FS_OK;
}

static int ramfs_op_mkdir(fs_t *fsroot, const char *path) {
    ramfs_fs_t *fs = (ramfs_fs_t *)fsroot->priv;
    ramfs_node_t *parent;
    char leaf[FS_MAX_NAME + 1];
    int r = resolve_parent(fs, path, &parent, leaf);
    if (r != FS_OK) return r;
    if (dir_lookup(parent, leaf)) return FS_ERR_EXIST;

    ramfs_node_t *n = node_new(leaf, true);
    if (!n) return FS_ERR_NOMEM;
    dir_link(parent, n);
    fs->node_count++;
    return FS_OK;
}

static int ramfs_op_unlink(fs_t *fsroot, const char *path) {
    ramfs_fs_t *fs = (ramfs_fs_t *)fsroot->priv;
    ramfs_node_t *n;
    int r = resolve(fs, path, &n);
    if (r != FS_OK) return r;
    if (n == fs->root) return FS_ERR_PARAM;
    if (n->is_dir) return FS_ERR_ISDIR;

    dir_unlink(n->parent ? n->parent : fs->root, n);
    if (n->open_count) n->unlinked = true;
    else node_free(fs, n);
    return FS_OK;
}

static int ramfs_op_rmdir(fs_t *fsroot, const char *path) {
    ramfs_fs_t *fs = (ramfs_fs_t *)fsroot->priv;
    ramfs_node_t *n;
    int r = resolve(fs, path, &n);
    if (r != FS_OK) return r;
    if (n == fs->root) return FS_ERR_PARAM;
    if (!n->is_dir) return FS_ERR_NOTDIR;
    if (n->first_child) return FS_ERR_NOTEMPTY;

    dir_unlink(n->parent, n);
    node_free(fs, n);
    return FS_OK;
}

static int ramfs_op_stat(fs_t *fsroot, const char *path, fs_dirent_t *out) {
    ramfs_fs_t *fs = (ramfs_fs_t *)fsroot->priv;
    ramfs_node_t *n;
    int r = resolve(fs, path, &n);
    if (r != FS_OK) return r;

    strncpy(out->name, n == fs->root ? "/" : n->name, FS_MAX_NAME);
    out->name[FS_MAX_NAME] = '\0';
    out->type = n->is_dir ? FS_ENTRY_DIR : FS_ENTRY_FILE;
    out->size = n->size;
    return FS_OK;
}

static int ramfs_op_unmount(fs_t *fsroot) {
    ramfs_fs_t *fs = (ramfs_fs_t *)fsroot->priv;
    if (!fs) return FS_OK;
    node_free_tree(fs, fs->root);
    kfree(fs);
    fsroot->priv = NULL;
    return FS_OK;
}

const fs_ops_t ramfs_ops = {
    .open = ramfs_op_open,
    .close = ramfs_op_close,
    .read = ramfs_op_read,
    .write = ramfs_op_write,
    .seek = ramfs_op_seek,
    .truncate = ramfs_op_truncate,
    .opendir = ramfs_op_opendir,
    .readdir = ramfs_op_readdir,
    .closedir = ramfs_op_closedir,
    .mkdir = ramfs_op_mkdir,
    .unlink = ramfs_op_unlink,
    .rmdir = ramfs_op_rmdir,
    .stat = ramfs_op_stat,
    .unmount = ramfs_op_unmount,
};

int ramfs_mount(fs_t *out) {
    if (!out) return FS_ERR_PARAM;

    ramfs_fs_t *fs = kmalloc(sizeof(ramfs_fs_t));
    if (!fs) return FS_ERR_NOMEM;
    memset(fs, 0, sizeof(*fs));

    fs->root = node_new("", true);
    if (!fs->root) { kfree(fs); return FS_ERR_NOMEM; }
    fs->node_count = 1;

    out->type = FS_TYPE_RAMFS;
    out->dev = NULL;
    out->ops = &ramfs_ops;
    out->priv = fs;
    strncpy(out->label, "ramfs", sizeof(out->label) - 1);
    out->label[sizeof(out->label) - 1] = '\0';
    fs_volume_lock_init(out);

    LOG_DEBUG("ramfs instance created");
    return FS_OK;
}
