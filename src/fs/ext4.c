#include "fs/ext4.h"

#include "components/Memory/heap.h"

#include <stdio.h>
#include <string.h>

#define ELOG(fmt, ...) printf_color(0xFF55FFFF, "[ext4] " fmt, ##__VA_ARGS__)
#define EERR(fmt, ...) printf_color(0xFFFF5555, "[ext4] ERR " fmt, ##__VA_ARGS__)

static int read_block(ext4_fs_t *fs, uint64_t block, void *buf) {
    if (fs->dev->read_sectors(fs->dev, block * fs->sectors_per_block, fs->sectors_per_block, buf) != 0)
        return FS_ERR_IO;
    return FS_OK;
}

static int read_group_desc(ext4_fs_t *fs, uint32_t group, ext4_group_desc_t *out) {
    uint64_t byte_off = (uint64_t)group * fs->desc_size;
    uint64_t block = fs->gdt_start_block + byte_off / fs->block_size;
    uint32_t off = (uint32_t)(byte_off % fs->block_size);

    uint8_t *buf = kmalloc(fs->block_size);
    if (!buf) return FS_ERR_NOMEM;
    if (read_block(fs, block, buf) != FS_OK) { kfree(buf); return FS_ERR_IO; }

    memset(out, 0, sizeof(*out));
    uint32_t copy = fs->desc_size < sizeof(*out) ? fs->desc_size : (uint32_t)sizeof(*out);
    memcpy(out, buf + off, copy);
    kfree(buf);
    return FS_OK;
}

static int read_inode(ext4_fs_t *fs, uint32_t inode_num, ext4_inode_raw_t *out) {
    if (inode_num == 0) return FS_ERR_PARAM;
    uint32_t group = (inode_num - 1) / fs->sb.s_inodes_per_group;
    uint32_t index = (inode_num - 1) % fs->sb.s_inodes_per_group;

    ext4_group_desc_t gd;
    if (read_group_desc(fs, group, &gd) != FS_OK) return FS_ERR_IO;

    uint64_t inode_table = gd.bg_inode_table_lo;
    if (fs->has_64bit) inode_table |= ((uint64_t)gd.bg_inode_table_hi << 32);

    uint64_t byte_off = (uint64_t)index * fs->sb.s_inode_size;
    uint64_t block = inode_table + byte_off / fs->block_size;
    uint32_t off = (uint32_t)(byte_off % fs->block_size);

    uint8_t *buf = kmalloc(fs->block_size);
    if (!buf) return FS_ERR_NOMEM;
    if (read_block(fs, block, buf) != FS_OK) { kfree(buf); return FS_ERR_IO; }
    memcpy(out, buf + off, sizeof(ext4_inode_raw_t));
    kfree(buf);
    return FS_OK;
}

static uint64_t inode_size(const ext4_inode_raw_t *inode) {
    return ((uint64_t)inode->i_size_high << 32) | inode->i_size_lo;
}

static int extent_find_block(ext4_fs_t *fs, const ext4_inode_raw_t *inode, uint32_t lblk,
                              uint64_t *out_pblk, bool *out_unwritten) {
    if (!(inode->i_flags & EXT4_INODE_FLAG_EXTENTS)) return FS_ERR_NOSUPP;

    uint8_t local[60];
    memcpy(local, inode->i_block, 60);
    uint8_t *buf = local;
    bool buf_is_heap = false;

    *out_pblk = 0;
    if (out_unwritten) *out_unwritten = false;

    for (;;) {
        ext4_extent_header_t eh;
        memcpy(&eh, buf, sizeof(eh));
        if (eh.eh_magic != EXT4_EXTENT_MAGIC) { if (buf_is_heap) kfree(buf); return FS_ERR_CORRUPT; }

        if (eh.eh_depth == 0) {
            for (uint16_t i = 0; i < eh.eh_entries; i++) {
                ext4_extent_t ext;
                memcpy(&ext, buf + 12 + (size_t)i * 12, sizeof(ext));
                uint32_t len = ext.ee_len & 0x7FFF;
                bool unwritten = (ext.ee_len & 0x8000) != 0;
                if (lblk >= ext.ee_block && lblk < ext.ee_block + len) {
                    uint64_t pblk = ((uint64_t)ext.ee_start_hi << 32) | ext.ee_start_lo;
                    *out_pblk = pblk + (lblk - ext.ee_block);
                    if (out_unwritten) *out_unwritten = unwritten;
                    if (buf_is_heap) kfree(buf);
                    return FS_OK;
                }
            }
            if (buf_is_heap) kfree(buf);
            return FS_OK;
        }

        int chosen = -1;
        for (uint16_t i = 0; i < eh.eh_entries; i++) {
            ext4_extent_idx_t idx;
            memcpy(&idx, buf + 12 + (size_t)i * 12, sizeof(idx));
            if (idx.ei_block <= lblk) chosen = i; else break;
        }
        if (chosen < 0) { if (buf_is_heap) kfree(buf); return FS_OK; }

        ext4_extent_idx_t idx;
        memcpy(&idx, buf + 12 + (size_t)chosen * 12, sizeof(idx));
        uint64_t next_block = ((uint64_t)idx.ei_leaf_hi << 32) | idx.ei_leaf_lo;

        uint8_t *newbuf = kmalloc(fs->block_size);
        if (!newbuf) { if (buf_is_heap) kfree(buf); return FS_ERR_NOMEM; }
        if (read_block(fs, next_block, newbuf) != FS_OK) { kfree(newbuf); if (buf_is_heap) kfree(buf); return FS_ERR_IO; }
        if (buf_is_heap) kfree(buf);
        buf = newbuf;
        buf_is_heap = true;
    }
}

static int64_t inode_read(ext4_fs_t *fs, const ext4_inode_raw_t *inode, uint64_t pos, void *out_buf, uint64_t size) {
    uint64_t fsize = inode_size(inode);
    if (pos >= fsize) return 0;
    uint64_t remaining = fsize - pos;
    if (size > remaining) size = remaining;

    uint8_t *scratch = kmalloc(fs->block_size);
    if (!scratch) return FS_ERR_NOMEM;

    uint8_t *out = (uint8_t *)out_buf;
    uint64_t done = 0;
    while (done < size) {
        uint32_t lblk = (uint32_t)((pos + done) / fs->block_size);
        uint32_t off = (uint32_t)((pos + done) % fs->block_size);
        uint64_t pblk = 0; bool unwritten = false;

        if (extent_find_block(fs, inode, lblk, &pblk, &unwritten) != FS_OK) break;

        uint64_t chunk = fs->block_size - off;
        if (chunk > size - done) chunk = size - done;

        if (pblk == 0 || unwritten) {
            memset(out + done, 0, (size_t)chunk);
        } else {
            if (read_block(fs, pblk, scratch) != FS_OK) break;
            memcpy(out + done, scratch + off, (size_t)chunk);
        }
        done += chunk;
    }

    kfree(scratch);
    return (int64_t)done;
}

static int dir_find(ext4_fs_t *fs, const ext4_inode_raw_t *dir, const char *name,
                     uint32_t *out_inode, uint8_t *out_type) {
    uint64_t dsize = inode_size(dir);
    uint8_t *blockbuf = kmalloc(fs->block_size);
    if (!blockbuf) return FS_ERR_NOMEM;

    size_t name_len = strlen(name);
    int ret = FS_ERR_NOENT;

    for (uint64_t pos = 0; pos < dsize; ) {
        uint32_t off = (uint32_t)(pos % fs->block_size);
        if (off == 0) {
            uint32_t lblk = (uint32_t)(pos / fs->block_size);
            uint64_t pblk = 0; bool unwritten = false;
            if (extent_find_block(fs, dir, lblk, &pblk, &unwritten) != FS_OK || pblk == 0 || unwritten) {
                pos += fs->block_size;
                continue;
            }
            if (read_block(fs, pblk, blockbuf) != FS_OK) { ret = FS_ERR_IO; break; }
        }

        ext4_dirent_raw_t *de = (ext4_dirent_raw_t *)(blockbuf + off);
        if (de->rec_len < 8) break;

        if (de->inode != 0 && de->name_len == name_len && memcmp(de->name, name, name_len) == 0) {
            *out_inode = de->inode;
            if (out_type) *out_type = de->file_type;
            ret = FS_OK;
            break;
        }

        pos += de->rec_len;
    }

    kfree(blockbuf);
    return ret;
}

#define EXT4_ROOT_INODE 2u

static int resolve_path_inode(ext4_fs_t *fs, const char *path, uint32_t *out_num, ext4_inode_raw_t *out_raw) {
    fs_path_parts_t pp;
    int r = fs_split_path(path, &pp);
    if (r != FS_OK) return r;

    uint32_t cur = EXT4_ROOT_INODE;
    ext4_inode_raw_t cur_raw;
    if (read_inode(fs, cur, &cur_raw) != FS_OK) return FS_ERR_IO;

    for (int i = 0; i < pp.count; i++) {
        if ((cur_raw.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return FS_ERR_NOTDIR;
        uint32_t child;
        r = dir_find(fs, &cur_raw, pp.comps[i], &child, NULL);
        if (r != FS_OK) return r;
        cur = child;
        if (read_inode(fs, cur, &cur_raw) != FS_OK) return FS_ERR_IO;
    }

    if (out_num) *out_num = cur;
    if (out_raw) *out_raw = cur_raw;
    return FS_OK;
}

static int ext4_op_open(fs_t *fsroot, const char *path, bool create, bool truncate_flag, fs_file_t *out) {
    (void)create; (void)truncate_flag;
    ext4_fs_t *fs = (ext4_fs_t *)fsroot->priv;
    if (create) return FS_ERR_NOSUPP;

    ext4_inode_raw_t raw;
    int r = resolve_path_inode(fs, path, NULL, &raw);
    if (r != FS_OK) return r;
    if ((raw.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) return FS_ERR_ISDIR;

    ext4_file_t *ef = kmalloc(sizeof(ext4_file_t));
    if (!ef) return FS_ERR_NOMEM;
    ef->fs = fs;
    ef->inode = raw;
    ef->size = inode_size(&raw);

    out->priv = ef;
    out->size = ef->size;
    out->pos = 0;
    out->writable = false;
    return FS_OK;
}

static int ext4_op_close(fs_file_t *f) {
    if (f->priv) kfree(f->priv);
    f->priv = NULL;
    return FS_OK;
}

static int64_t ext4_op_read(fs_file_t *f, void *buf, uint64_t size) {
    ext4_file_t *ef = (ext4_file_t *)f->priv;
    int64_t n = inode_read(ef->fs, &ef->inode, f->pos, buf, size);
    if (n > 0) f->pos += (uint64_t)n;
    return n;
}

static int64_t ext4_op_write(fs_file_t *f, const void *buf, uint64_t size) {
    (void)f; (void)buf; (void)size;
    return FS_ERR_NOSUPP;
}

static int ext4_op_seek(fs_file_t *f, uint64_t pos) { f->pos = pos; return FS_OK; }
static int ext4_op_truncate(fs_file_t *f, uint64_t size) { (void)f; (void)size; return FS_ERR_NOSUPP; }

static int ext4_op_opendir(fs_t *fsroot, const char *path, fs_dir_t *out) {
    ext4_fs_t *fs = (ext4_fs_t *)fsroot->priv;
    ext4_inode_raw_t raw;
    int r = resolve_path_inode(fs, path, NULL, &raw);
    if (r != FS_OK) return r;
    if ((raw.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return FS_ERR_NOTDIR;

    ext4_diriter_t *it = kmalloc(sizeof(ext4_diriter_t));
    if (!it) return FS_ERR_NOMEM;
    it->fs = fs;
    it->dir_inode = raw;
    it->pos = 0;
    it->loaded_block_index = (uint32_t)-1;
    it->blockbuf = kmalloc(fs->block_size);
    if (!it->blockbuf) { kfree(it); return FS_ERR_NOMEM; }

    out->priv = it;
    return FS_OK;
}

static int ext4_op_readdir(fs_dir_t *dir, fs_dirent_t *out) {
    ext4_diriter_t *it = (ext4_diriter_t *)dir->priv;
    ext4_fs_t *fs = it->fs;
    uint64_t dsize = inode_size(&it->dir_inode);

    while (it->pos < dsize) {
        uint32_t lblk = (uint32_t)(it->pos / fs->block_size);
        uint32_t off = (uint32_t)(it->pos % fs->block_size);

        if (it->loaded_block_index != lblk) {
            uint64_t pblk = 0; bool unwritten = false;
            if (extent_find_block(fs, &it->dir_inode, lblk, &pblk, &unwritten) != FS_OK ||
                pblk == 0 || unwritten) {
                it->pos = (uint64_t)(lblk + 1) * fs->block_size;
                continue;
            }
            if (read_block(fs, pblk, it->blockbuf) != FS_OK) return FS_ERR_IO;
            it->loaded_block_index = lblk;
        }

        ext4_dirent_raw_t *de = (ext4_dirent_raw_t *)(it->blockbuf + off);
        if (de->rec_len < 8) return FS_ERR_CORRUPT;
        it->pos += de->rec_len;

        if (de->inode == 0 || de->name_len == 0) continue;

        uint32_t n = de->name_len;
        memcpy(out->name, de->name, n);
        out->name[n] = '\0';

        if (fs->has_filetype && de->file_type != 0) {
            out->type = (de->file_type == 2) ? FS_ENTRY_DIR : FS_ENTRY_FILE;
            out->size = 0;
        } else {
            ext4_inode_raw_t child;
            if (read_inode(fs, de->inode, &child) == FS_OK) {
                out->type = ((child.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) ? FS_ENTRY_DIR : FS_ENTRY_FILE;
                out->size = inode_size(&child);
            } else {
                out->type = FS_ENTRY_FILE;
                out->size = 0;
            }
        }
        return FS_OK;
    }
    return FS_ERR_EOF;
}

static int ext4_op_closedir(fs_dir_t *dir) {
    ext4_diriter_t *it = (ext4_diriter_t *)dir->priv;
    if (it) {
        if (it->blockbuf) kfree(it->blockbuf);
        kfree(it);
    }
    dir->priv = NULL;
    return FS_OK;
}

static int ext4_op_stat(fs_t *fsroot, const char *path, fs_dirent_t *out) {
    ext4_fs_t *fs = (ext4_fs_t *)fsroot->priv;

    fs_path_parts_t pp;
    int r = fs_split_path(path, &pp);
    if (r != FS_OK) return r;

    ext4_inode_raw_t raw;
    r = resolve_path_inode(fs, path, NULL, &raw);
    if (r != FS_OK) return r;

    const char *name = pp.count ? pp.comps[pp.count - 1] : "/";
    strncpy(out->name, name, FS_MAX_NAME); out->name[FS_MAX_NAME] = '\0';
    out->type = ((raw.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) ? FS_ENTRY_DIR : FS_ENTRY_FILE;
    out->size = inode_size(&raw);
    return FS_OK;
}

static int ext4_op_notsupported_path(fs_t *fsroot, const char *path) { (void)fsroot; (void)path; return FS_ERR_NOSUPP; }

static int ext4_op_unmount(fs_t *fsroot) {
    if (fsroot->priv) kfree(fsroot->priv);
    fsroot->priv = NULL;
    return FS_OK;
}

const fs_ops_t ext4_ops = {
    .open = ext4_op_open,
    .close = ext4_op_close,
    .read = ext4_op_read,
    .write = ext4_op_write,
    .seek = ext4_op_seek,
    .truncate = ext4_op_truncate,
    .opendir = ext4_op_opendir,
    .readdir = ext4_op_readdir,
    .closedir = ext4_op_closedir,
    .mkdir = ext4_op_notsupported_path,
    .unlink = ext4_op_notsupported_path,
    .rmdir = ext4_op_notsupported_path,
    .stat = ext4_op_stat,
    .unmount = ext4_op_unmount,
};

static int load_superblock(struct block_device *dev, uint32_t sector_size, ext4_superblock_t *out) {

    uint32_t read_bytes = 2048;
    uint32_t sectors = (read_bytes + sector_size - 1) / sector_size;
    uint8_t *buf = kmalloc((size_t)sectors * sector_size);
    if (!buf) return FS_ERR_NOMEM;

    uint64_t start_lba = EXT4_SUPERBLOCK_OFFSET / sector_size;
    if (dev->read_sectors(dev, start_lba, sectors, buf) != 0) { kfree(buf); return FS_ERR_IO; }

    uint32_t sb_off_in_buf = EXT4_SUPERBLOCK_OFFSET % sector_size;
    memcpy(out, buf + sb_off_in_buf, sizeof(ext4_superblock_t));
    kfree(buf);

    if (out->s_magic != EXT4_SUPER_MAGIC) return FS_ERR_CORRUPT;
    return FS_OK;
}

int ext4_probe(struct block_device *dev) {
    if (!dev) return FS_ERR_PARAM;
    uint32_t sector_size = dev->sector_size ? dev->sector_size : 512;
    ext4_superblock_t sb;
    int r = load_superblock(dev, sector_size, &sb);
    if (r != FS_OK) return r;
    if (!(sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS)) return FS_ERR_NOSUPP;
    return FS_OK;
}

int ext4_mount(struct block_device *dev, fs_t *out) {
    if (!dev || !out) return FS_ERR_PARAM;
    uint32_t sector_size = dev->sector_size ? dev->sector_size : 512;

    ext4_superblock_t sb;
    int r = load_superblock(dev, sector_size, &sb);
    if (r != FS_OK) return r;

    if (!(sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS)) {
        EERR("volume without extent inodes (ext2/3 format too old) is not supported\n");
        return FS_ERR_NOSUPP;
    }

    ext4_fs_t *fs = kmalloc(sizeof(ext4_fs_t));
    if (!fs) return FS_ERR_NOMEM;
    memset(fs, 0, sizeof(*fs));

    fs->dev = dev;
    fs->sb = sb;
    fs->block_size = 1024u << sb.s_log_block_size;
    fs->sectors_per_block = fs->block_size / sector_size;
    if (fs->sectors_per_block == 0) fs->sectors_per_block = 1;

    fs->has_64bit = (sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0;
    fs->has_filetype = (sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE) != 0;
    fs->desc_size = fs->has_64bit ? (sb.s_desc_size ? sb.s_desc_size : 64) : 32;
    fs->gdt_start_block = sb.s_first_data_block + 1;

    uint64_t blocks_count = sb.s_blocks_count_lo;
    if (fs->has_64bit) blocks_count |= ((uint64_t)sb.s_blocks_count_hi << 32);
    fs->num_groups = (uint32_t)((blocks_count + sb.s_blocks_per_group - 1) / sb.s_blocks_per_group);

    out->type = FS_TYPE_EXT4;
    out->dev = dev;
    out->ops = &ext4_ops;
    out->priv = fs;
    strncpy(out->label, sb.s_volume_name, 16);
    out->label[16] = '\0';

    ELOG("mounted (read-only) volume '%s', block=%u bytes, groups=%u\n",
         sb.s_volume_name[0] ? sb.s_volume_name : "(no label)",
         (unsigned)fs->block_size, (unsigned)fs->num_groups);

    return FS_OK;
}
