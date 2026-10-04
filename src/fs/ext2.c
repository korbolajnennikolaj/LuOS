#include "fs/ext2.h"

#include "components/logger.h"
#include "components/Memory/heap.h"

#include <string.h>

#define EXT2_DESC_SIZE_V0 32
#define EXT2_DIR_ROUND 4
#define EXT2_DIR_REC_LEN(n) ((uint16_t)(((n) + 8 + EXT2_DIR_ROUND - 1) & ~(EXT2_DIR_ROUND - 1)))
#define EXT2_DIR_MIN_REC_LEN 8

static int read_block(ext2_fs_t *fs, uint64_t block, void *buf) {
    if (block >= fs->blocks_count) return FS_ERR_PARAM;
    if (fs->dev->read_sectors(fs->dev, block * fs->sectors_per_block, fs->sectors_per_block, buf) != 0) {
        LOG_ERROR("%s: failed to read block %llu", fs->dev->name, (unsigned long long)block);
        return FS_ERR_IO;
    }
    return FS_OK;
}

static int write_block(ext2_fs_t *fs, uint64_t block, const void *buf) {
    if (block >= fs->blocks_count) return FS_ERR_PARAM;
    if (fs->dev->write_sectors(fs->dev, block * fs->sectors_per_block, fs->sectors_per_block, (void *)buf) != 0) {
        LOG_ERROR("%s: failed to write block %llu", fs->dev->name, (unsigned long long)block);
        return FS_ERR_IO;
    }
    return FS_OK;
}

static uint64_t group_first_block(ext2_fs_t *fs, uint32_t group) {
    return (uint64_t)group * fs->blocks_per_group + fs->first_data_block;
}

static uint32_t blocks_in_group(ext2_fs_t *fs, uint32_t group) {
    uint64_t first = group_first_block(fs, group);
    if (fs->blocks_count <= first) return 0;
    uint64_t n = fs->blocks_count - first;
    if (n > fs->blocks_per_group) n = fs->blocks_per_group;
    return (uint32_t)n;
}

static int gdt_block_for(ext2_fs_t *fs, uint32_t group, uint32_t *off_in_block) {
    uint64_t byte_off = (uint64_t)group * fs->desc_size;
    uint64_t gdt_start = (uint64_t)fs->first_data_block + 1;
    uint64_t block = gdt_start + byte_off / fs->block_size;
    *off_in_block = (uint32_t)(byte_off % fs->block_size);
    return block;
}

static int read_group_desc(ext2_fs_t *fs, uint32_t group, ext2_group_desc_t *out) {
    if (group >= fs->num_groups) return FS_ERR_PARAM;
    uint32_t off;
    uint64_t block = gdt_block_for(fs, group, &off);

    if (fs->dev->read_sectors(fs->dev, block * fs->sectors_per_block, fs->sectors_per_block, fs->scratch) != 0) {
        LOG_ERROR("%s: failed to read group descriptor %u", fs->dev->name, group);
        return FS_ERR_IO;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out, fs->scratch + off, EXT2_DESC_SIZE_V0);
    return FS_OK;
}

static int write_group_desc(ext2_fs_t *fs, uint32_t group, const ext2_group_desc_t *gd) {
    if (group >= fs->num_groups) return FS_ERR_PARAM;
    uint32_t off;
    uint64_t block = gdt_block_for(fs, group, &off);

    if (fs->dev->read_sectors(fs->dev, block * fs->sectors_per_block, fs->sectors_per_block, fs->scratch) != 0)
        return FS_ERR_IO;
    memcpy(fs->scratch + off, gd, EXT2_DESC_SIZE_V0);
    if (fs->dev->write_sectors(fs->dev, block * fs->sectors_per_block, fs->sectors_per_block, fs->scratch) != 0)
        return FS_ERR_IO;
    return FS_OK;
}

static int write_superblock(ext2_fs_t *fs) {
    uint64_t block = fs->first_data_block;
    uint32_t off = 0;
    if (fs->block_size == 1024) {
        off = 0;
    } else {
        block = 0;
        off = EXT2_SUPERBLOCK_OFFSET;
    }

    uint32_t need = (off + sizeof(ext2_superblock_t) + fs->block_size - 1) / fs->block_size;
    uint8_t *buf = kmalloc((size_t)need * fs->block_size);
    if (!buf) return FS_ERR_NOMEM;

    uint32_t nsec = need * fs->sectors_per_block;
    if (fs->dev->read_sectors(fs->dev, block * fs->sectors_per_block, nsec, buf) != 0) {
        kfree(buf);
        return FS_ERR_IO;
    }
    memcpy(buf + off, &fs->sb, sizeof(ext2_superblock_t));
    int r = fs->dev->write_sectors(fs->dev, block * fs->sectors_per_block, nsec, buf) != 0 ? FS_ERR_IO : FS_OK;
    kfree(buf);
    return r;
}

static uint32_t inode_group(ext2_fs_t *fs, uint32_t ino) {
    return (ino - 1) / fs->inodes_per_group;
}

static uint32_t inode_index(ext2_fs_t *fs, uint32_t ino) {
    return (ino - 1) % fs->inodes_per_group;
}

static int read_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *out) {
    if (ino == 0 || ino > fs->sb.s_inodes_count) return FS_ERR_PARAM;

    ext2_group_desc_t gd;
    int r = read_group_desc(fs, inode_group(fs, ino), &gd);
    if (r != FS_OK) return r;

    uint64_t byte_off = (uint64_t)inode_index(fs, ino) * fs->inode_size;
    uint64_t block = gd.bg_inode_table + byte_off / fs->block_size;
    uint32_t off = (uint32_t)(byte_off % fs->block_size);

    if (fs->dev->read_sectors(fs->dev, block * fs->sectors_per_block, fs->sectors_per_block, fs->scratch) != 0) {
        LOG_ERROR("%s: failed to read inode %u", fs->dev->name, ino);
        return FS_ERR_IO;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out, fs->scratch + off, sizeof(ext2_inode_t) <= fs->inode_size ? sizeof(ext2_inode_t) : fs->inode_size);
    return FS_OK;
}

static int write_inode(ext2_fs_t *fs, uint32_t ino, const ext2_inode_t *in) {
    if (ino == 0 || ino > fs->sb.s_inodes_count) return FS_ERR_PARAM;

    ext2_group_desc_t gd;
    int r = read_group_desc(fs, inode_group(fs, ino), &gd);
    if (r != FS_OK) return r;

    uint64_t byte_off = (uint64_t)inode_index(fs, ino) * fs->inode_size;
    uint64_t block = gd.bg_inode_table + byte_off / fs->block_size;
    uint32_t off = (uint32_t)(byte_off % fs->block_size);

    if (fs->dev->read_sectors(fs->dev, block * fs->sectors_per_block, fs->sectors_per_block, fs->scratch) != 0)
        return FS_ERR_IO;
    memcpy(fs->scratch + off, in, sizeof(ext2_inode_t) <= fs->inode_size ? sizeof(ext2_inode_t) : fs->inode_size);
    if (fs->dev->write_sectors(fs->dev, block * fs->sectors_per_block, fs->sectors_per_block, fs->scratch) != 0)
        return FS_ERR_IO;
    return FS_OK;
}

static int bitmap_bit(ext2_fs_t *fs, uint64_t bitmap_block, uint32_t bit, int *value) {
    if (fs->dev->read_sectors(fs->dev, bitmap_block * fs->sectors_per_block, fs->sectors_per_block, fs->scratch) != 0)
        return FS_ERR_IO;
    *value = (fs->scratch[bit / 8] >> (bit % 8)) & 1;
    return FS_OK;
}

static int bitmap_set(ext2_fs_t *fs, uint64_t bitmap_block, uint32_t bit, int on) {
    if (fs->dev->read_sectors(fs->dev, bitmap_block * fs->sectors_per_block, fs->sectors_per_block, fs->scratch) != 0)
        return FS_ERR_IO;
    uint32_t byte = bit / 8;
    uint32_t mask = 1u << (bit % 8);
    if (on) fs->scratch[byte] |= (uint8_t)mask;
    else fs->scratch[byte] &= (uint8_t)~mask;
    if (fs->dev->write_sectors(fs->dev, bitmap_block * fs->sectors_per_block, fs->sectors_per_block, fs->scratch) != 0)
        return FS_ERR_IO;
    return FS_OK;
}

static int sb_update_free(ext2_fs_t *fs, int blocks_delta, int inodes_delta) {
    if (blocks_delta) {
        if (blocks_delta > 0 && (uint32_t)blocks_delta > fs->sb.s_free_blocks_count) return FS_ERR_CORRUPT;
        if (blocks_delta < 0 && (uint32_t)(-blocks_delta) > 0xFFFFFFFFu - fs->sb.s_free_blocks_count) return FS_ERR_CORRUPT;
        fs->sb.s_free_blocks_count = (uint32_t)(fs->sb.s_free_blocks_count + blocks_delta);
    }
    if (inodes_delta) {
        if (inodes_delta > 0 && (uint32_t)inodes_delta > fs->sb.s_free_inodes_count) return FS_ERR_CORRUPT;
        if (inodes_delta < 0 && (uint32_t)(-inodes_delta) > 0xFFFFFFFFu - fs->sb.s_free_inodes_count) return FS_ERR_CORRUPT;
        fs->sb.s_free_inodes_count = (uint32_t)(fs->sb.s_free_inodes_count + inodes_delta);
    }
    fs->dirty = true;
    return write_superblock(fs);
}

static int alloc_inode(ext2_fs_t *fs, uint32_t hint_group, bool is_dir, uint32_t *out_ino) {
    for (uint32_t pass = 0; pass < 2; pass++) {
        for (uint32_t g = 0; g < fs->num_groups; g++) {
            uint32_t group = hint_group + g;
            if (group >= fs->num_groups) group -= fs->num_groups;

            ext2_group_desc_t gd;
            if (read_group_desc(fs, group, &gd) != FS_OK) return FS_ERR_IO;
            if (gd.bg_free_inodes_count == 0) continue;

            uint64_t bitmap = gd.bg_inode_bitmap;
            if (fs->dev->read_sectors(fs->dev, bitmap * fs->sectors_per_block, fs->sectors_per_block, fs->scratch) != 0)
                return FS_ERR_IO;

            uint32_t limit = fs->inodes_per_group;
            for (uint32_t i = 0; i < limit; i++) {
                if ((fs->scratch[i / 8] >> (i % 8)) & 1) continue;

                fs->scratch[i / 8] |= (uint8_t)(1u << (i % 8));
                if (fs->dev->write_sectors(fs->dev, bitmap * fs->sectors_per_block, fs->sectors_per_block, fs->scratch) != 0)
                    return FS_ERR_IO;

                gd.bg_free_inodes_count--;
                if (is_dir) gd.bg_used_dirs_count++;
                if (write_group_desc(fs, group, &gd) != FS_OK) return FS_ERR_IO;

                int r = sb_update_free(fs, 0, -1);
                if (r != FS_OK) return r;

                *out_ino = group * fs->inodes_per_group + i + 1;
                return FS_OK;
            }
        }
    }
    return FS_ERR_NOSPACE;
}

static int free_inode(ext2_fs_t *fs, uint32_t ino, bool was_dir) {
    uint32_t group = inode_group(fs, ino);
    uint32_t index = inode_index(fs, ino);

    ext2_group_desc_t gd;
    if (read_group_desc(fs, group, &gd) != FS_OK) return FS_ERR_IO;

    int v = 0;
    if (bitmap_bit(fs, gd.bg_inode_bitmap, index, &v) != FS_OK) return FS_ERR_IO;
    if (!v) return FS_ERR_CORRUPT;

    if (bitmap_set(fs, gd.bg_inode_bitmap, index, 0) != FS_OK) return FS_ERR_IO;

    gd.bg_free_inodes_count++;
    if (was_dir && gd.bg_used_dirs_count > 0) gd.bg_used_dirs_count--;
    if (write_group_desc(fs, group, &gd) != FS_OK) return FS_ERR_IO;

    return sb_update_free(fs, 0, 1);
}

static int alloc_block(ext2_fs_t *fs, uint32_t hint_group, uint32_t *out_block) {
    for (uint32_t pass = 0; pass < 2; pass++) {
        for (uint32_t g = 0; g < fs->num_groups; g++) {
            uint32_t group = hint_group + g;
            if (group >= fs->num_groups) group -= fs->num_groups;

            uint32_t in_group = blocks_in_group(fs, group);
            if (in_group == 0) continue;

            ext2_group_desc_t gd;
            if (read_group_desc(fs, group, &gd) != FS_OK) return FS_ERR_IO;
            if (gd.bg_free_blocks_count == 0) continue;

            uint64_t bitmap = gd.bg_block_bitmap;
            if (fs->dev->read_sectors(fs->dev, bitmap * fs->sectors_per_block, fs->sectors_per_block, fs->scratch) != 0)
                return FS_ERR_IO;

            for (uint32_t i = 0; i < in_group; i++) {
                if ((fs->scratch[i / 8] >> (i % 8)) & 1) continue;

                fs->scratch[i / 8] |= (uint8_t)(1u << (i % 8));
                if (fs->dev->write_sectors(fs->dev, bitmap * fs->sectors_per_block, fs->sectors_per_block, fs->scratch) != 0)
                    return FS_ERR_IO;

                gd.bg_free_blocks_count--;
                if (write_group_desc(fs, group, &gd) != FS_OK) return FS_ERR_IO;

                int r = sb_update_free(fs, -1, 0);
                if (r != FS_OK) return r;

                *out_block = (uint32_t)(group_first_block(fs, group) + i);
                return FS_OK;
            }
        }
    }
    return FS_ERR_NOSPACE;
}

static int free_block(ext2_fs_t *fs, uint32_t block) {
    if (block < fs->first_data_block || block >= fs->blocks_count) return FS_ERR_PARAM;

    uint32_t group = (uint32_t)((block - fs->first_data_block) / fs->blocks_per_group);
    uint32_t index = (uint32_t)((block - fs->first_data_block) % fs->blocks_per_group);
    if (group >= fs->num_groups || index >= blocks_in_group(fs, group)) return FS_ERR_PARAM;

    ext2_group_desc_t gd;
    if (read_group_desc(fs, group, &gd) != FS_OK) return FS_ERR_IO;

    int v = 0;
    if (bitmap_bit(fs, gd.bg_block_bitmap, index, &v) != FS_OK) return FS_ERR_IO;
    if (!v) return FS_ERR_CORRUPT;

    if (bitmap_set(fs, gd.bg_block_bitmap, index, 0) != FS_OK) return FS_ERR_IO;

    gd.bg_free_blocks_count++;
    if (write_group_desc(fs, group, &gd) != FS_OK) return FS_ERR_IO;

    return sb_update_free(fs, 1, 0);
}

static int inode_alloc_block(ext2_fs_t *fs, ext2_inode_t *inode, uint32_t group_hint, uint32_t *out_block) {
    uint32_t b = 0;
    int r = alloc_block(fs, group_hint, &b);
    if (r != FS_OK) return r;
    inode->i_blocks_lo += fs->block_size / 512u;
    *out_block = b;
    return FS_OK;
}

static int zero_block(ext2_fs_t *fs, uint32_t block) {
    memset(fs->scratch, 0, fs->block_size);
    return write_block(fs, block, fs->scratch);
}

static int ind_walk(ext2_fs_t *fs, uint32_t *slot, uint32_t index, int level, bool alloc,
                    ext2_inode_t *inode, uint32_t group_hint, uint32_t *out_block) {
    if (*slot == 0) {
        if (!alloc) { *out_block = 0; return FS_OK; }
        uint32_t b = 0;
        int r = inode_alloc_block(fs, inode, group_hint, &b);
        if (r != FS_OK) return r;
        r = zero_block(fs, b);
        if (r != FS_OK) return r;
        *slot = b;
    }

    uint8_t *buf = kmalloc(fs->block_size);
    if (!buf) return FS_ERR_NOMEM;
    if (read_block(fs, *slot, buf) != FS_OK) { kfree(buf); return FS_ERR_IO; }

    uint32_t *ents = (uint32_t *)buf;
    int r = FS_OK;

    if (level == 1) {
        if (ents[index] == 0 && alloc) {
            uint32_t b = 0;
            r = inode_alloc_block(fs, inode, group_hint, &b);
            if (r == FS_OK) {
                ents[index] = b;
                r = write_block(fs, *slot, buf);
            }
        }
        *out_block = ents[index];
    } else {
        uint32_t per = fs->block_size / 4u;
        uint32_t hi = index / per;
        uint32_t lo = index % per;
        if (hi >= per) { kfree(buf); return FS_ERR_PARAM; }
        r = ind_walk(fs, &ents[hi], lo, level - 1, alloc, inode, group_hint, out_block);
        if (alloc && r == FS_OK) {
            r = write_block(fs, *slot, buf);
        }
    }

    kfree(buf);
    return r;
}

static int block_ptr_get(ext2_fs_t *fs, ext2_inode_t *inode, uint32_t lblk, bool alloc, uint32_t *out_block) {
    uint32_t per = fs->block_size / 4u;
    uint32_t group_hint = 0;

    if (lblk < EXT2_NDIR_BLOCKS) {
        if (inode->i_block[lblk] == 0 && alloc) {
            uint32_t b = 0;
            int r = inode_alloc_block(fs, inode, group_hint, &b);
            if (r != FS_OK) return r;
            inode->i_block[lblk] = b;
        }
        *out_block = inode->i_block[lblk];
        return FS_OK;
    }

    uint32_t off = lblk - EXT2_NDIR_BLOCKS;
    uint32_t slot = 0;
    int r;
    if (off < per) {
        slot = inode->i_block[EXT2_IND_BLOCK];
        r = ind_walk(fs, &slot, off, 1, alloc, inode, group_hint, out_block);
        inode->i_block[EXT2_IND_BLOCK] = slot;
        return r;
    }
    off -= per;
    if (off < per * per) {
        slot = inode->i_block[EXT2_DIND_BLOCK];
        r = ind_walk(fs, &slot, off, 2, alloc, inode, group_hint, out_block);
        inode->i_block[EXT2_DIND_BLOCK] = slot;
        return r;
    }
    off -= per * per;
    if (off < per * per * per) {
        slot = inode->i_block[EXT2_TIND_BLOCK];
        r = ind_walk(fs, &slot, off, 3, alloc, inode, group_hint, out_block);
        inode->i_block[EXT2_TIND_BLOCK] = slot;
        return r;
    }
    return FS_ERR_NOSUPP;
}

static uint64_t inode_size(const ext2_inode_t *inode) {
    return ((uint64_t)inode->i_size_high << 32) | inode->i_size_lo;
}

static void inode_set_size(ext2_inode_t *inode, uint64_t size) {
    inode->i_size_lo = (uint32_t)size;
    inode->i_size_high = (uint32_t)(size >> 32);
}

static int64_t ext2_read_data(ext2_fs_t *fs, ext2_inode_t *inode, uint64_t pos, void *out_buf, uint64_t size) {
    uint64_t fsize = inode_size(inode);
    if (pos >= fsize) return 0;
    if (size > fsize - pos) size = fsize - pos;

    uint8_t *out = (uint8_t *)out_buf;
    uint64_t done = 0;

    while (done < size) {
        uint32_t lblk = (uint32_t)((pos + done) / fs->block_size);
        uint32_t off = (uint32_t)((pos + done) % fs->block_size);
        uint32_t pblk = 0;

        int r = block_ptr_get(fs, inode, lblk, false, &pblk);
        if (r != FS_OK) return r;

        uint64_t chunk = fs->block_size - off;
        if (chunk > size - done) chunk = size - done;

        if (pblk == 0) {
            memset(out + done, 0, (size_t)chunk);
        } else {
            if (read_block(fs, pblk, fs->scratch) != FS_OK) return FS_ERR_IO;
            memcpy(out + done, fs->scratch + off, (size_t)chunk);
        }
        done += chunk;
    }
    return (int64_t)done;
}

static int ext2_write_data(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *inode, uint64_t pos, const void *buf, uint64_t size) {
    const uint8_t *in = (const uint8_t *)buf;
    uint64_t done = 0;

    while (done < size) {
        uint32_t lblk = (uint32_t)((pos + done) / fs->block_size);
        uint32_t off = (uint32_t)((pos + done) % fs->block_size);
        uint32_t pblk = 0;

        int r = block_ptr_get(fs, inode, lblk, true, &pblk);
        if (r != FS_OK) return r;
        if (pblk == 0) return FS_ERR_NOSPACE;

        uint64_t chunk = fs->block_size - off;
        if (chunk > size - done) chunk = size - done;

        if (off == 0 && chunk == fs->block_size) {
            if (write_block(fs, pblk, in + done) != FS_OK) return FS_ERR_IO;
        } else {
            if (read_block(fs, pblk, fs->scratch) != FS_OK) return FS_ERR_IO;
            memcpy(fs->scratch + off, in + done, (size_t)chunk);
            if (write_block(fs, pblk, fs->scratch) != FS_OK) return FS_ERR_IO;
        }
        done += chunk;
    }

    if (pos + size > inode_size(inode)) inode_set_size(inode, pos + size);
    return write_inode(fs, ino, inode);
}

static int truncate_to(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *inode, uint64_t new_size) {
    uint64_t old_size = inode_size(inode);
    if (new_size >= old_size) {
        inode_set_size(inode, new_size);
        return write_inode(fs, ino, inode);
    }

    uint32_t first_free_lblk = (uint32_t)((new_size + fs->block_size - 1) / fs->block_size);
    uint32_t per = fs->block_size / 4u;
    uint64_t max_lblk = (old_size + fs->block_size - 1) / fs->block_size;

    for (uint64_t lblk = first_free_lblk; lblk < max_lblk; lblk++) {
        uint32_t pblk = 0;
        if (block_ptr_get(fs, inode, (uint32_t)lblk, false, &pblk) != FS_OK) continue;
        if (pblk != 0) {
            if (free_block(fs, pblk) == FS_OK) inode->i_blocks_lo -= fs->block_size / 512u;
        }
    }

    uint32_t keep_indirect = first_free_lblk;
    if (keep_indirect > EXT2_NDIR_BLOCKS) {
        keep_indirect -= EXT2_NDIR_BLOCKS;
        if (keep_indirect > per) keep_indirect = per;
    } else {
        keep_indirect = 0;
    }

    if (keep_indirect == 0) {
        static const int slots[3] = { EXT2_IND_BLOCK, EXT2_DIND_BLOCK, EXT2_TIND_BLOCK };
        for (int s = 0; s < 3; s++) {
            uint32_t root = inode->i_block[slots[s]];
            if (root == 0) continue;

            if (s == 0) {
                if (free_block(fs, root) == FS_OK) inode->i_blocks_lo -= fs->block_size / 512u;
            } else {
                uint8_t *buf = kmalloc(fs->block_size);
                if (buf) {
                    if (read_block(fs, root, buf) == FS_OK) {
                        uint32_t *ents = (uint32_t *)buf;
                        uint32_t count = fs->block_size / 4u;
                        for (uint32_t i = 0; i < count; i++) {
                            if (ents[i]) {
                                if (s == 1) {
                                    if (free_block(fs, ents[i]) == FS_OK) inode->i_blocks_lo -= fs->block_size / 512u;
                                } else {
                                    uint8_t *buf2 = kmalloc(fs->block_size);
                                    if (buf2 && read_block(fs, ents[i], buf2) == FS_OK) {
                                        uint32_t *ents2 = (uint32_t *)buf2;
                                        for (uint32_t j = 0; j < count; j++) {
                                            if (ents2[j] && free_block(fs, ents2[j]) == FS_OK)
                                                inode->i_blocks_lo -= fs->block_size / 512u;
                                        }
                                    }
                                    if (buf2) kfree(buf2);
                                    if (free_block(fs, ents[i]) == FS_OK) inode->i_blocks_lo -= fs->block_size / 512u;
                                }
                            }
                        }
                    }
                    kfree(buf);
                }
                if (free_block(fs, root) == FS_OK) inode->i_blocks_lo -= fs->block_size / 512u;
            }
            inode->i_block[slots[s]] = 0;
        }
    }

    for (uint32_t i = first_free_lblk; i < EXT2_NDIR_BLOCKS && i < max_lblk; i++) {
        inode->i_block[i] = 0;
    }

    inode_set_size(inode, new_size);
    return write_inode(fs, ino, inode);
}

static int dir_lookup(ext2_fs_t *fs, ext2_inode_t *dir, const char *name, size_t name_len,
                      uint32_t *out_ino, uint64_t *out_block, uint32_t *out_off) {
    uint64_t dsize = inode_size(dir);
    uint8_t *buf = kmalloc(fs->block_size);
    if (!buf) return FS_ERR_NOMEM;

    int ret = FS_ERR_NOENT;
    for (uint64_t pos = 0; pos < dsize; ) {
        uint32_t lblk = (uint32_t)(pos / fs->block_size);
        uint32_t pblk = 0;
        if (block_ptr_get(fs, dir, lblk, false, &pblk) != FS_OK || pblk == 0) {
            pos += fs->block_size;
            continue;
        }
        if (read_block(fs, pblk, buf) != FS_OK) { ret = FS_ERR_IO; break; }

        uint32_t off = 0;
        while (off + EXT2_DIR_MIN_REC_LEN <= fs->block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(buf + off);
            if (de->rec_len < EXT2_DIR_MIN_REC_LEN || off + de->rec_len > fs->block_size) break;

            if (de->inode != 0 && de->name_len == name_len && memcmp(de->name, name, name_len) == 0) {
                if (out_ino) *out_ino = de->inode;
                if (out_block) *out_block = pblk;
                if (out_off) *out_off = off;
                ret = FS_OK;
                goto done;
            }
            off += de->rec_len;
        }
        pos += fs->block_size;
    }

done:
    kfree(buf);
    return ret;
}

static int dir_entry_type(ext2_fs_t *fs, uint32_t ino, uint8_t ft) {
    if (fs->has_filetype && ft != EXT2_FT_UNKNOWN) {
        return (ft == EXT2_FT_DIR) ? FS_ENTRY_DIR : FS_ENTRY_FILE;
    }
    ext2_inode_t child;
    if (read_inode(fs, ino, &child) != FS_OK) return FS_ENTRY_FILE;
    return ((child.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) ? FS_ENTRY_DIR : FS_ENTRY_FILE;
}

static int dir_first_free(ext2_fs_t *fs, uint32_t dir_ino, ext2_inode_t *dir,
                          const char *name, size_t name_len, uint32_t target_ino, uint8_t ft) {
    uint16_t need = EXT2_DIR_REC_LEN(name_len);
    uint64_t dsize = inode_size(dir);
    uint8_t *buf = kmalloc(fs->block_size);
    if (!buf) return FS_ERR_NOMEM;

    for (uint64_t pos = 0; pos < dsize; ) {
        uint32_t lblk = (uint32_t)(pos / fs->block_size);
        uint32_t pblk = 0;
        if (block_ptr_get(fs, dir, lblk, false, &pblk) != FS_OK || pblk == 0) {
            pos += fs->block_size;
            continue;
        }
        if (read_block(fs, pblk, buf) != FS_OK) { kfree(buf); return FS_ERR_IO; }

        uint32_t off = 0;
        while (off + EXT2_DIR_MIN_REC_LEN <= fs->block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(buf + off);
            if (de->rec_len < EXT2_DIR_MIN_REC_LEN || off + de->rec_len > fs->block_size) break;

            uint16_t used = de->inode ? EXT2_DIR_REC_LEN(de->name_len) : 8;

            if (de->inode == 0 && de->rec_len >= need) {
                uint16_t old_len = de->rec_len;
                memset(buf + off, 0, old_len);
                de->inode = target_ino;
                de->rec_len = need;
                de->name_len = (uint8_t)name_len;
                de->file_type = ft;
                memcpy(de->name, name, name_len);
                if (old_len - need >= EXT2_DIR_MIN_REC_LEN) {
                    ext2_dirent_t *rest = (ext2_dirent_t *)(buf + off + need);
                    rest->inode = 0;
                    rest->rec_len = (uint16_t)(old_len - need);
                } else {
                    de->rec_len = old_len;
                }
                int w = write_block(fs, pblk, buf);
                kfree(buf);
                return w;
            }

            if (de->inode != 0 && off + de->rec_len == fs->block_size &&
                de->rec_len - used >= need) {
                uint16_t old = de->rec_len;
                de->rec_len = used;
                ext2_dirent_t *ne = (ext2_dirent_t *)(buf + off + used);
                memset(ne, 0, old - used);
                ne->inode = target_ino;
                ne->rec_len = (uint16_t)(old - used);
                ne->name_len = (uint8_t)name_len;
                ne->file_type = ft;
                memcpy(ne->name, name, name_len);
                int w = write_block(fs, pblk, buf);
                kfree(buf);
                return w;
            }

            off += de->rec_len;
        }
        pos += fs->block_size;
    }
    kfree(buf);

    uint32_t lblk = (uint32_t)(dsize / fs->block_size);
    uint32_t pblk = 0;
    int r = block_ptr_get(fs, dir, lblk, true, &pblk);
    if (r != FS_OK) return r;
    if (pblk == 0) return FS_ERR_NOSPACE;

    uint8_t *nb = kmalloc(fs->block_size);
    if (!nb) return FS_ERR_NOMEM;
    memset(nb, 0, fs->block_size);
    ext2_dirent_t *de = (ext2_dirent_t *)nb;
    de->inode = target_ino;
    de->rec_len = (uint16_t)fs->block_size;
    de->name_len = (uint8_t)name_len;
    de->file_type = ft;
    memcpy(de->name, name, name_len);

    r = write_block(fs, pblk, nb);
    kfree(nb);
    if (r != FS_OK) return r;

    inode_set_size(dir, (uint64_t)(lblk + 1) * fs->block_size);
    return write_inode(fs, dir_ino, dir);
}

static int dir_remove_entry(ext2_fs_t *fs, ext2_inode_t *dir, const char *name, size_t name_len) {
    uint64_t dsize = inode_size(dir);
    uint8_t *buf = kmalloc(fs->block_size);
    if (!buf) return FS_ERR_NOMEM;

    int ret = FS_ERR_NOENT;
    for (uint64_t pos = 0; pos < dsize; ) {
        uint32_t lblk = (uint32_t)(pos / fs->block_size);
        uint32_t pblk = 0;
        if (block_ptr_get(fs, dir, lblk, false, &pblk) != FS_OK || pblk == 0) {
            pos += fs->block_size;
            continue;
        }
        if (read_block(fs, pblk, buf) != FS_OK) { ret = FS_ERR_IO; break; }

        uint32_t off = 0;
        uint32_t prev_off = 0;
        while (off + EXT2_DIR_MIN_REC_LEN <= fs->block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(buf + off);
            if (de->rec_len < EXT2_DIR_MIN_REC_LEN || off + de->rec_len > fs->block_size) break;

            if (de->inode != 0 && de->name_len == name_len && memcmp(de->name, name, name_len) == 0) {
                if (off > 0) {
                    ext2_dirent_t *prev = (ext2_dirent_t *)(buf + prev_off);
                    prev->rec_len += de->rec_len;
                    de->inode = 0;
                } else {
                    de->inode = 0;
                }
                ret = write_block(fs, pblk, buf);
                kfree(buf);
                return ret;
            }

            prev_off = off;
            off += de->rec_len;
        }
        pos += fs->block_size;
    }

    kfree(buf);
    return ret;
}

static int dir_is_empty(ext2_fs_t *fs, ext2_inode_t *dir) {
    uint64_t dsize = inode_size(dir);
    uint8_t *buf = kmalloc(fs->block_size);
    if (!buf) return FS_ERR_NOMEM;

    int ret = FS_OK;
    for (uint64_t pos = 0; pos < dsize && ret == FS_OK; ) {
        uint32_t lblk = (uint32_t)(pos / fs->block_size);
        uint32_t pblk = 0;
        if (block_ptr_get(fs, dir, lblk, false, &pblk) != FS_OK || pblk == 0) {
            pos += fs->block_size;
            continue;
        }
        if (read_block(fs, pblk, buf) != FS_OK) { ret = FS_ERR_IO; break; }

        uint32_t off = 0;
        while (off + EXT2_DIR_MIN_REC_LEN <= fs->block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(buf + off);
            if (de->rec_len < EXT2_DIR_MIN_REC_LEN || off + de->rec_len > fs->block_size) break;

            if (de->inode != 0) {
                if (!(de->name_len == 1 && de->name[0] == '.') &&
                    !(de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.')) {
                    ret = FS_ERR_NOTEMPTY;
                    break;
                }
            }
            off += de->rec_len;
        }
        pos += fs->block_size;
    }

    kfree(buf);
    return ret;
}

static int resolve_path(ext2_fs_t *fs, const char *path, uint32_t *out_ino, ext2_inode_t *out_inode) {
    fs_path_parts_t pp;
    int r = fs_split_path(path, &pp);
    if (r != FS_OK) return r;

    uint32_t cur = EXT2_ROOT_INO;
    ext2_inode_t cur_raw;
    if (read_inode(fs, cur, &cur_raw) != FS_OK) return FS_ERR_IO;

    for (int i = 0; i < pp.count; i++) {
        if ((cur_raw.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return FS_ERR_NOTDIR;
        uint32_t child = 0;
        r = dir_lookup(fs, &cur_raw, pp.comps[i], strlen(pp.comps[i]), &child, NULL, NULL);
        if (r != FS_OK) return r;
        cur = child;
        if (read_inode(fs, cur, &cur_raw) != FS_OK) return FS_ERR_IO;
    }

    if (out_ino) *out_ino = cur;
    if (out_inode) *out_inode = cur_raw;
    return FS_OK;
}

static int resolve_parent(ext2_fs_t *fs, const char *path, fs_path_parts_t *pp,
                          uint32_t *out_ino, ext2_inode_t *out_inode,
                          const char **out_name, size_t *out_name_len) {
    int r = fs_split_path(path, pp);
    if (r != FS_OK) return r;
    if (pp->count == 0) return FS_ERR_PARAM;

    uint32_t cur = EXT2_ROOT_INO;
    ext2_inode_t cur_raw;
    if (read_inode(fs, cur, &cur_raw) != FS_OK) return FS_ERR_IO;

    for (int i = 0; i < pp->count - 1; i++) {
        if ((cur_raw.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return FS_ERR_NOTDIR;
        uint32_t child = 0;
        r = dir_lookup(fs, &cur_raw, pp->comps[i], strlen(pp->comps[i]), &child, NULL, NULL);
        if (r != FS_OK) return r;
        cur = child;
        if (read_inode(fs, cur, &cur_raw) != FS_OK) return FS_ERR_IO;
    }

    if ((cur_raw.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return FS_ERR_NOTDIR;

    *out_ino = cur;
    *out_inode = cur_raw;
    *out_name = pp->comps[pp->count - 1];
    *out_name_len = strlen(pp->comps[pp->count - 1]);
    return FS_OK;
}

static int ext2_op_open(fs_t *fsroot, const char *path, bool create, bool truncate_flag, fs_file_t *out) {
    ext2_fs_t *fs = (ext2_fs_t *)fsroot->priv;
    int r;

    {
        fs_path_parts_t pp;
        uint32_t parent_ino = 0;
        ext2_inode_t parent;
        const char *name = NULL;
        size_t name_len = 0;
        r = resolve_parent(fs, path, &pp, &parent_ino, &parent, &name, &name_len);
        if (r != FS_OK) return r;

        uint32_t child = 0;
        r = dir_lookup(fs, &parent, name, name_len, &child, NULL, NULL);

        if (r == FS_OK) {
            ext2_inode_t raw;
            if (read_inode(fs, child, &raw) != FS_OK) return FS_ERR_IO;
            if ((raw.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return FS_ERR_ISDIR;

            if (truncate_flag) {
                r = truncate_to(fs, child, &raw, 0);
                if (r != FS_OK) return r;
            }

            ext2_file_t *ef = kmalloc(sizeof(ext2_file_t));
            if (!ef) return FS_ERR_NOMEM;
            ef->fs = fs;
            ef->ino = child;
            ef->inode = raw;
            ef->size = inode_size(&raw);
            ef->writable = true;

            out->priv = ef;
            out->size = ef->size;
            out->pos = 0;
            out->writable = true;
            return FS_OK;
        }

        if (r != FS_ERR_NOENT) return r;
        if (!create) return FS_ERR_NOENT;

        uint32_t new_ino = 0;
        r = alloc_inode(fs, inode_group(fs, parent_ino), false, &new_ino);
        if (r != FS_OK) return r;

        ext2_inode_t raw;
        memset(&raw, 0, sizeof(raw));
        raw.i_mode = EXT2_S_IFREG | 0644;
        raw.i_links_count = 1;
        inode_set_size(&raw, 0);
        if (write_inode(fs, new_ino, &raw) != FS_OK) {
            free_inode(fs, new_ino, false);
            return FS_ERR_IO;
        }

        r = dir_first_free(fs, parent_ino, &parent, name, name_len, new_ino,
                           fs->has_filetype ? EXT2_FT_REG_FILE : EXT2_FT_UNKNOWN);
        if (r != FS_OK) {
            free_inode(fs, new_ino, false);
            return r;
        }

        ext2_file_t *ef = kmalloc(sizeof(ext2_file_t));
        if (!ef) { free_inode(fs, new_ino, false); return FS_ERR_NOMEM; }
        ef->fs = fs;
        ef->ino = new_ino;
        ef->inode = raw;
        ef->size = 0;
        ef->writable = true;

        out->priv = ef;
        out->size = 0;
        out->pos = 0;
        out->writable = true;
        return FS_OK;
    }
    return FS_ERR_PARAM;
}

static int ext2_op_close(fs_file_t *f) {
    ext2_file_t *ef = (ext2_file_t *)f->priv;
    if (ef) {
        inode_set_size(&ef->inode, ef->size);
        write_inode(ef->fs, ef->ino, &ef->inode);
        kfree(ef);
    }
    f->priv = NULL;
    return FS_OK;
}

static int64_t ext2_op_read(fs_file_t *f, void *buf, uint64_t size) {
    ext2_file_t *ef = (ext2_file_t *)f->priv;
    if (f->pos > ef->size) return 0;
    int64_t n = ext2_read_data(ef->fs, &ef->inode, f->pos, buf, size);
    if (n > 0) f->pos += (uint64_t)n;
    return n;
}

static int64_t ext2_op_write(fs_file_t *f, const void *buf, uint64_t size) {
    ext2_file_t *ef = (ext2_file_t *)f->priv;
    int r = ext2_write_data(ef->fs, ef->ino, &ef->inode, f->pos, buf, size);
    if (r != FS_OK) return r;
    f->pos += size;
    if (f->pos > ef->size) ef->size = f->pos;
    f->size = ef->size;
    return (int64_t)size;
}

static int ext2_op_seek(fs_file_t *f, uint64_t pos) {
    f->pos = pos;
    return FS_OK;
}

static int ext2_op_truncate(fs_file_t *f, uint64_t size) {
    ext2_file_t *ef = (ext2_file_t *)f->priv;
    int r = truncate_to(ef->fs, ef->ino, &ef->inode, size);
    if (r != FS_OK) return r;
    ef->size = size;
    f->size = size;
    if (f->pos > size) f->pos = size;
    return FS_OK;
}

static int ext2_op_opendir(fs_t *fsroot, const char *path, fs_dir_t *out) {
    ext2_fs_t *fs = (ext2_fs_t *)fsroot->priv;

    uint32_t ino = 0;
    ext2_inode_t raw;
    int r = resolve_path(fs, path, &ino, &raw);
    if (r != FS_OK) return r;
    if ((raw.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return FS_ERR_NOTDIR;

    ext2_diriter_t *it = kmalloc(sizeof(ext2_diriter_t));
    if (!it) return FS_ERR_NOMEM;
    it->fs = fs;
    it->ino = ino;
    it->inode = raw;
    it->pos = 0;
    it->size = inode_size(&raw);
    it->blockbuf = kmalloc(fs->block_size);
    it->loaded_lblk = (uint32_t)-1;
    if (!it->blockbuf) { kfree(it); return FS_ERR_NOMEM; }

    out->priv = it;
    return FS_OK;
}

static int ext2_op_readdir(fs_dir_t *dir, fs_dirent_t *out) {
    ext2_diriter_t *it = (ext2_diriter_t *)dir->priv;
    ext2_fs_t *fs = it->fs;

    while (it->pos < it->size) {
        uint32_t lblk = (uint32_t)(it->pos / fs->block_size);
        uint32_t off = (uint32_t)(it->pos % fs->block_size);

        if (it->loaded_lblk != lblk) {
            uint32_t pblk = 0;
            if (block_ptr_get(fs, &it->inode, lblk, false, &pblk) != FS_OK || pblk == 0) {
                it->pos = (uint64_t)(lblk + 1) * fs->block_size;
                continue;
            }
            if (read_block(fs, pblk, it->blockbuf) != FS_OK) return FS_ERR_IO;
            it->loaded_lblk = lblk;
        }

        ext2_dirent_t *de = (ext2_dirent_t *)(it->blockbuf + off);
        if (de->rec_len < EXT2_DIR_MIN_REC_LEN || off + de->rec_len > fs->block_size) {
            it->pos = (uint64_t)(lblk + 1) * fs->block_size;
            continue;
        }
        it->pos += de->rec_len;

        if (de->inode == 0 || de->name_len == 0) continue;
        if (de->name_len == 1 && de->name[0] == '.') continue;
        if (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.') continue;

        uint32_t n = de->name_len;
        memcpy(out->name, de->name, n);
        out->name[n] = '\0';
        out->type = (dir_entry_type(fs, de->inode, de->file_type) == FS_ENTRY_DIR) ? FS_ENTRY_DIR : FS_ENTRY_FILE;
        out->size = 0;
        if (out->type == FS_ENTRY_FILE) {
            ext2_inode_t child;
            if (read_inode(fs, de->inode, &child) == FS_OK) out->size = inode_size(&child);
        }
        return FS_OK;
    }
    return FS_ERR_EOF;
}

static int ext2_op_closedir(fs_dir_t *dir) {
    ext2_diriter_t *it = (ext2_diriter_t *)dir->priv;
    if (it) {
        if (it->blockbuf) kfree(it->blockbuf);
        kfree(it);
    }
    dir->priv = NULL;
    return FS_OK;
}

static int ext2_op_mkdir(fs_t *fsroot, const char *path) {
    ext2_fs_t *fs = (ext2_fs_t *)fsroot->priv;

    fs_path_parts_t pp;
    uint32_t parent_ino = 0;
    ext2_inode_t parent;
    const char *name = NULL;
    size_t name_len = 0;
    int r = resolve_parent(fs, path, &pp, &parent_ino, &parent, &name, &name_len);
    if (r != FS_OK) return r;

    uint32_t child = 0;
    if (dir_lookup(fs, &parent, name, name_len, &child, NULL, NULL) == FS_OK) return FS_ERR_EXIST;

    uint32_t new_ino = 0;
    r = alloc_inode(fs, inode_group(fs, parent_ino), true, &new_ino);
    if (r != FS_OK) return r;

    ext2_inode_t raw;
    memset(&raw, 0, sizeof(raw));
    raw.i_mode = EXT2_S_IFDIR | 0755;
    raw.i_links_count = 2;
    inode_set_size(&raw, fs->block_size);

    uint32_t pblk = 0;
    r = block_ptr_get(fs, &raw, 0, true, &pblk);
    if (r != FS_OK || pblk == 0) { free_inode(fs, new_ino, true); return FS_ERR_NOSPACE; }

    uint8_t *buf = kmalloc(fs->block_size);
    if (!buf) { free_inode(fs, new_ino, true); return FS_ERR_NOMEM; }
    memset(buf, 0, fs->block_size);

    ext2_dirent_t *dot = (ext2_dirent_t *)buf;
    dot->inode = new_ino;
    dot->rec_len = EXT2_DIR_REC_LEN(1);
    dot->name_len = 1;
    dot->file_type = fs->has_filetype ? EXT2_FT_DIR : EXT2_FT_UNKNOWN;
    dot->name[0] = '.';

    ext2_dirent_t *dotdot = (ext2_dirent_t *)(buf + dot->rec_len);
    dotdot->inode = parent_ino;
    dotdot->rec_len = (uint16_t)(fs->block_size - dot->rec_len);
    dotdot->name_len = 2;
    dotdot->file_type = fs->has_filetype ? EXT2_FT_DIR : EXT2_FT_UNKNOWN;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';

    r = write_block(fs, pblk, buf);
    kfree(buf);
    if (r != FS_OK) { free_inode(fs, new_ino, true); return r; }

    if (write_inode(fs, new_ino, &raw) != FS_OK) { free_inode(fs, new_ino, true); return FS_ERR_IO; }

    r = dir_first_free(fs, parent_ino, &parent, name, name_len, new_ino,
                       fs->has_filetype ? EXT2_FT_DIR : EXT2_FT_UNKNOWN);
    if (r != FS_OK) { free_inode(fs, new_ino, true); return r; }

    parent.i_links_count++;
    return write_inode(fs, parent_ino, &parent);
}

static int ext2_op_unlink(fs_t *fsroot, const char *path) {
    ext2_fs_t *fs = (ext2_fs_t *)fsroot->priv;

    fs_path_parts_t pp;
    uint32_t parent_ino = 0;
    ext2_inode_t parent;
    const char *name = NULL;
    size_t name_len = 0;
    int r = resolve_parent(fs, path, &pp, &parent_ino, &parent, &name, &name_len);
    if (r != FS_OK) return r;

    uint32_t child = 0;
    r = dir_lookup(fs, &parent, name, name_len, &child, NULL, NULL);
    if (r != FS_OK) return r;

    ext2_inode_t raw;
    if (read_inode(fs, child, &raw) != FS_OK) return FS_ERR_IO;
    if ((raw.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return FS_ERR_ISDIR;

    r = dir_remove_entry(fs, &parent, name, name_len);
    if (r != FS_OK) return r;

    if (raw.i_links_count > 0) raw.i_links_count--;
    if (raw.i_links_count == 0) {
        truncate_to(fs, child, &raw, 0);
        raw.i_dtime = 1;
        write_inode(fs, child, &raw);
        return free_inode(fs, child, false);
    }
    return write_inode(fs, child, &raw);
}

static int ext2_op_rmdir(fs_t *fsroot, const char *path) {
    ext2_fs_t *fs = (ext2_fs_t *)fsroot->priv;

    fs_path_parts_t pp;
    uint32_t parent_ino = 0;
    ext2_inode_t parent;
    const char *name = NULL;
    size_t name_len = 0;
    int r = resolve_parent(fs, path, &pp, &parent_ino, &parent, &name, &name_len);
    if (r != FS_OK) return r;

    uint32_t child = 0;
    r = dir_lookup(fs, &parent, name, name_len, &child, NULL, NULL);
    if (r != FS_OK) return r;

    ext2_inode_t raw;
    if (read_inode(fs, child, &raw) != FS_OK) return FS_ERR_IO;
    if ((raw.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return FS_ERR_NOTDIR;

    int e = dir_is_empty(fs, &raw);
    if (e == FS_ERR_NOTEMPTY) return e;
    if (e != FS_OK) return e;

    r = dir_remove_entry(fs, &parent, name, name_len);
    if (r != FS_OK) return r;

    truncate_to(fs, child, &raw, 0);
    raw.i_dtime = 1;
    write_inode(fs, child, &raw);

    r = free_inode(fs, child, true);
    if (r != FS_OK) return r;

    if (parent.i_links_count > 1) parent.i_links_count--;
    return write_inode(fs, parent_ino, &parent);
}

static int ext2_op_stat(fs_t *fsroot, const char *path, fs_dirent_t *out) {
    ext2_fs_t *fs = (ext2_fs_t *)fsroot->priv;

    fs_path_parts_t pp;
    int r = fs_split_path(path, &pp);
    if (r != FS_OK) return r;

    uint32_t ino = 0;
    ext2_inode_t raw;
    r = resolve_path(fs, path, &ino, &raw);
    if (r != FS_OK) return r;

    const char *name = pp.count ? pp.comps[pp.count - 1] : "/";
    strncpy(out->name, name, FS_MAX_NAME);
    out->name[FS_MAX_NAME] = '\0';
    out->type = ((raw.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) ? FS_ENTRY_DIR : FS_ENTRY_FILE;
    out->size = inode_size(&raw);
    return FS_OK;
}

static int ext2_op_unmount(fs_t *fsroot) {
    ext2_fs_t *fs = (ext2_fs_t *)fsroot->priv;
    if (fs) {
        fs->sb.s_state = 1;
        fs->sb.s_mnt_count++;
        write_superblock(fs);
        if (fs->scratch) kfree(fs->scratch);
        kfree(fs);
    }
    fsroot->priv = NULL;
    return FS_OK;
}

const fs_ops_t ext2_ops = {
    .open = ext2_op_open,
    .close = ext2_op_close,
    .read = ext2_op_read,
    .write = ext2_op_write,
    .seek = ext2_op_seek,
    .truncate = ext2_op_truncate,
    .opendir = ext2_op_opendir,
    .readdir = ext2_op_readdir,
    .closedir = ext2_op_closedir,
    .mkdir = ext2_op_mkdir,
    .unlink = ext2_op_unlink,
    .rmdir = ext2_op_rmdir,
    .stat = ext2_op_stat,
    .unmount = ext2_op_unmount,
};

static int load_superblock(struct block_device *dev, uint32_t sector_size, ext2_superblock_t *out) {
    uint32_t read_bytes = 2048;
    uint32_t sectors = (read_bytes + sector_size - 1) / sector_size;
    uint8_t *buf = kmalloc((size_t)sectors * sector_size);
    if (!buf) return FS_ERR_NOMEM;

    uint64_t start_lba = EXT2_SUPERBLOCK_OFFSET / sector_size;
    if (dev->read_sectors(dev, start_lba, sectors, buf) != 0) {
        kfree(buf);
        return FS_ERR_IO;
    }

    uint32_t sb_off = EXT2_SUPERBLOCK_OFFSET % sector_size;
    memcpy(out, buf + sb_off, sizeof(ext2_superblock_t));
    kfree(buf);

    if (out->s_magic != EXT2_SUPER_MAGIC) return FS_ERR_CORRUPT;
    return FS_OK;
}

int ext2_probe(struct block_device *dev) {
    if (!dev) return FS_ERR_PARAM;
    uint32_t ss = dev->sector_size ? dev->sector_size : 512;

    ext2_superblock_t sb;
    int r = load_superblock(dev, ss, &sb);
    if (r != FS_OK) return r;

    if (sb.s_feature_incompat & EXT2_FEATURE_INCOMPAT_EXTENTS) return FS_ERR_NOSUPP;
    if (sb.s_feature_incompat & EXT2_FEATURE_INCOMPAT_COMPRESSION) return FS_ERR_NOSUPP;
    if (sb.s_feature_incompat & EXT2_FEATURE_INCOMPAT_64BIT) return FS_ERR_NOSUPP;
    if (sb.s_feature_incompat & EXT2_FEATURE_INCOMPAT_MMP) return FS_ERR_NOSUPP;
    if (sb.s_feature_incompat & EXT3_FEATURE_INCOMPAT_JOURNAL_DEV) return FS_ERR_NOSUPP;
    if (sb.s_log_block_size > 2) return FS_ERR_NOSUPP;
    return FS_OK;
}

int ext2_is_ext3(struct block_device *dev) {
    if (!dev) return 0;
    uint32_t ss = dev->sector_size ? dev->sector_size : 512;

    ext2_superblock_t sb;
    if (load_superblock(dev, ss, &sb) != FS_OK) return 0;
    if (sb.s_feature_compat & EXT2_FEATURE_COMPAT_HAS_JOURNAL) return 1;
    if (sb.s_feature_incompat & EXT3_FEATURE_INCOMPAT_RECOVER) return 1;
    return 0;
}

int ext2_mount(struct block_device *dev, fs_t *out) {
    if (!dev || !out) return FS_ERR_PARAM;
    uint32_t ss = dev->sector_size ? dev->sector_size : 512;

    ext2_superblock_t sb;
    int r = load_superblock(dev, ss, &sb);
    if (r != FS_OK) return r;

    if (sb.s_feature_incompat & EXT3_FEATURE_INCOMPAT_RECOVER) {
        LOG_ERROR("%s: ext3 journal replay required, not supported", dev->name);
        return FS_ERR_NOSUPP;
    }

    ext2_fs_t *fs = kmalloc(sizeof(ext2_fs_t));
    if (!fs) return FS_ERR_NOMEM;
    memset(fs, 0, sizeof(*fs));

    fs->dev = dev;
    fs->sb = sb;
    fs->block_size = 1024u << sb.s_log_block_size;
    fs->sectors_per_block = fs->block_size / ss;
    if (fs->sectors_per_block == 0) fs->sectors_per_block = 1;
    fs->first_data_block = sb.s_first_data_block;
    fs->blocks_per_group = sb.s_blocks_per_group;
    fs->inodes_per_group = sb.s_inodes_per_group;
    fs->blocks_count = sb.s_blocks_count;
    fs->inode_size = (sb.s_rev_level >= 1 && sb.s_inode_size >= sizeof(ext2_inode_t)) ? sb.s_inode_size : EXT2_GOOD_OLD_INODE_SIZE;
    fs->desc_size = (sb.s_rev_level >= 1 && sb.s_desc_size >= EXT2_DESC_SIZE_V0) ? sb.s_desc_size : EXT2_DESC_SIZE_V0;
    fs->has_filetype = (sb.s_feature_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) != 0;
    fs->has_journal = (sb.s_feature_compat & EXT2_FEATURE_COMPAT_HAS_JOURNAL) != 0;

    if (fs->blocks_per_group == 0 || fs->inodes_per_group == 0 || fs->block_size > 4096) {
        kfree(fs);
        return FS_ERR_CORRUPT;
    }

    uint64_t data_blocks = fs->blocks_count > fs->first_data_block ? fs->blocks_count - fs->first_data_block : 0;
    fs->num_groups = (uint32_t)((data_blocks + fs->blocks_per_group - 1) / fs->blocks_per_group);
    if (fs->num_groups == 0) { kfree(fs); return FS_ERR_CORRUPT; }

    fs->scratch = kmalloc(fs->block_size);
    if (!fs->scratch) { kfree(fs); return FS_ERR_NOMEM; }

    fs->sb.s_state = 0;
    fs->sb.s_mnt_count++;
    fs->dirty = true;
    write_superblock(fs);

    out->type = fs->has_journal ? FS_TYPE_EXT3 : FS_TYPE_EXT2;
    out->dev = dev;
    out->ops = &ext2_ops;
    out->priv = fs;
    strncpy(out->label, sb.s_volume_name, 16);
    out->label[16] = '\0';

    LOG_DEBUG("mounted (read-write) volume '%s', block=%u bytes, groups=%u%s",
         sb.s_volume_name[0] ? sb.s_volume_name : "(no label)",
         (unsigned)fs->block_size, (unsigned)fs->num_groups,
         fs->has_journal ? ", ext3 journal ignored" : "");

    return FS_OK;
}

static int fmt_write_inode(struct block_device *dev, uint32_t ss, uint32_t bs,
                           uint32_t itable_start, uint32_t inode_size, uint32_t ino, const ext2_inode_t *in) {
    uint64_t byte_off = (uint64_t)(ino - 1) * inode_size;
    uint32_t block = itable_start + (uint32_t)(byte_off / bs);
    uint32_t off = (uint32_t)(byte_off % bs);
    uint32_t spb = bs / ss;

    uint8_t *b = kmalloc(bs);
    if (!b) return FS_ERR_NOMEM;
    if (dev->read_sectors(dev, (uint64_t)block * spb, spb, b) != 0) { kfree(b); return FS_ERR_IO; }
    memcpy(b + off, in, sizeof(*in));
    int r = dev->write_sectors(dev, (uint64_t)block * spb, spb, b) != 0 ? FS_ERR_IO : FS_OK;
    kfree(b);
    return r;
}

int ext2_format(struct block_device *dev, const char *label, int is_ext3) {
    if (!dev || !dev->write_sectors) return FS_ERR_PARAM;
    uint32_t ss = dev->sector_size ? dev->sector_size : 512;
    uint64_t total = dev->sector_count * ss;
    if (total < 1024u * 1024u) return FS_ERR_NOSUPP;

    uint32_t bs = total >= 16ull * 1024 * 1024 ? 4096 : 1024;
    if (ss > 512 && bs < ss) bs = 4096;
    uint32_t log_bs = bs == 1024 ? 0 : (bs == 2048 ? 1 : 2);
    uint32_t first_data_block = bs == 1024 ? 1 : 0;
    uint64_t blocks_count = total / bs;
    uint32_t bpg = bs * 8;
    uint64_t data_blocks = blocks_count - first_data_block;
    uint32_t groups = (uint32_t)((data_blocks + bpg - 1) / bpg);
    if (groups == 0) return FS_ERR_NOSUPP;

    uint64_t inodes_wanted = total / 8192;
    if (inodes_wanted < 16) inodes_wanted = 16;
    if (inodes_wanted > 0x1000000ull) inodes_wanted = 0x1000000ull;
    uint32_t ipg = (uint32_t)((inodes_wanted + groups - 1) / groups);
    if (ipg == 0) ipg = 1;
    uint32_t inodes_count = ipg * groups;

    uint32_t inode_size = EXT2_GOOD_OLD_INODE_SIZE;
    uint32_t itable_blocks = (ipg * inode_size + bs - 1) / bs;
    uint32_t gdt_blocks = (groups * EXT2_DESC_SIZE_V0 + bs - 1) / bs;

    uint64_t gdt_start = first_data_block + 1;
    uint32_t meta0 = (uint32_t)(gdt_start + gdt_blocks);
    uint32_t data0 = meta0 + 2 + itable_blocks;

    uint64_t used = (uint64_t)(data0 - first_data_block) + 2 + (uint64_t)(groups - 1) * (2 + itable_blocks);

    ext2_superblock_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.s_inodes_count = inodes_count;
    sb.s_blocks_count = (uint32_t)blocks_count;
    sb.s_free_blocks_count = (uint32_t)(blocks_count - used);
    sb.s_free_inodes_count = inodes_count - 11;
    sb.s_first_data_block = first_data_block;
    sb.s_log_block_size = log_bs;
    sb.s_blocks_per_group = bpg;
    sb.s_inodes_per_group = ipg;
    sb.s_magic = EXT2_SUPER_MAGIC;
    sb.s_state = 1;
    sb.s_errors = 1;
    sb.s_minor_rev_level = 0;
    sb.s_creator_os = 0;
    sb.s_rev_level = 1;
    sb.s_first_ino = EXT2_GOOD_OLD_FIRST_INO;
    sb.s_inode_size = (uint16_t)inode_size;
    sb.s_block_group_nr = 0;
    sb.s_feature_compat = is_ext3 ? EXT2_FEATURE_COMPAT_HAS_JOURNAL : 0;
    sb.s_feature_incompat = EXT2_FEATURE_INCOMPAT_FILETYPE;
    sb.s_feature_ro_compat = 0;
    uint32_t ser = fs_new_serial();
    for (int i = 0; i < 4; i++) sb.s_hash_seed[i] = ser * (uint32_t)(i + 1);
    sb.s_def_hash_version = 0;
    const char *vol = label && label[0] ? label : "LUOS";
    strncpy(sb.s_volume_name, vol, 16);
    sb.s_volume_name[15] = '\0';
    strncpy(sb.s_last_mounted, "LuOS", sizeof(sb.s_last_mounted) - 1);
    sb.s_max_mnt_count = 20;
    sb.s_mkfs_time = 0;

    uint8_t *buf = kmalloc(bs);
    uint8_t *gd_buf = kmalloc((size_t)gdt_blocks * bs);
    if (!buf || !gd_buf) { if (buf) kfree(buf); if (gd_buf) kfree(gd_buf); return FS_ERR_NOMEM; }

    memset(gd_buf, 0, (size_t)gdt_blocks * bs);
    for (uint32_t g = 0; g < groups; g++) {
        ext2_group_desc_t *gd = (ext2_group_desc_t *)(gd_buf + (size_t)g * EXT2_DESC_SIZE_V0);
        uint64_t group_first = (uint64_t)g * bpg + first_data_block;
        if (g == 0) {
            gd->bg_block_bitmap = meta0;
            gd->bg_inode_bitmap = meta0 + 1;
            gd->bg_inode_table = meta0 + 2;
        } else {
            gd->bg_block_bitmap = (uint32_t)group_first;
            gd->bg_inode_bitmap = (uint32_t)group_first + 1;
            gd->bg_inode_table = (uint32_t)group_first + 2;
        }
        uint32_t in_group_meta = (g == 0) ? (data0 - first_data_block + 2) : (2 + itable_blocks);
        uint32_t blocks_in_g = blocks_count - group_first < bpg ? (uint32_t)(blocks_count - group_first) : bpg;
        gd->bg_free_blocks_count = (uint16_t)(blocks_in_g > in_group_meta ? blocks_in_g - in_group_meta : 0);
        gd->bg_free_inodes_count = (uint16_t)(g == 0 ? ipg - 11 : ipg);
        gd->bg_used_dirs_count = (uint16_t)(g == 0 ? 1 : 0);
    }


    uint32_t spb = bs / ss;
    int r = FS_OK;

    memset(buf, 0, bs);
    if (bs == 1024) {
        if (dev->write_sectors(dev, 0, spb, buf) != 0) r = FS_ERR_IO;
        memcpy(buf, &sb, sizeof(sb));
        if (r == FS_OK && dev->write_sectors(dev, spb, spb, buf) != 0) r = FS_ERR_IO;
    } else {
        memcpy(buf + EXT2_SUPERBLOCK_OFFSET, &sb, sizeof(sb));
        if (dev->write_sectors(dev, 0, spb, buf) != 0) r = FS_ERR_IO;
    }

    if (r == FS_OK && dev->write_sectors(dev, gdt_start * spb, (uint32_t)gdt_blocks * spb, gd_buf) != 0)
        r = FS_ERR_IO;

    for (uint32_t g = 0; g < groups && r == FS_OK; g++) {
        ext2_group_desc_t *gd = (ext2_group_desc_t *)(gd_buf + (size_t)g * EXT2_DESC_SIZE_V0);

        memset(buf, 0, bs);
        uint32_t bits = (g == 0) ? (data0 - first_data_block + 2) : (2 + itable_blocks);
        for (uint32_t i = 0; i < bits; i++) {
            buf[i / 8] |= (uint8_t)(1u << (i % 8));
        }
        if (dev->write_sectors(dev, (uint64_t)gd->bg_block_bitmap * spb, spb, buf) != 0) { r = FS_ERR_IO; break; }

        memset(buf, 0, bs);
        if (g == 0) {
            for (uint32_t i = 0; i < 11; i++) {
                buf[i / 8] |= (uint8_t)(1u << (i % 8));
            }
        }
        if (dev->write_sectors(dev, (uint64_t)gd->bg_inode_bitmap * spb, spb, buf) != 0) { r = FS_ERR_IO; break; }

        memset(buf, 0, bs);
        for (uint32_t i = 0; i < itable_blocks; i++) {
            if (dev->write_sectors(dev, (uint64_t)gd->bg_inode_table * spb + (uint64_t)i * spb, spb, buf) != 0) { r = FS_ERR_IO; break; }
        }
        if (r != FS_OK) break;
    }

    kfree(gd_buf);
    if (r != FS_OK) { kfree(buf); return r; }

    uint32_t root_block = data0;
    uint32_t lf_block = data0 + 1;

    memset(buf, 0, bs);
    ext2_dirent_t *de = (ext2_dirent_t *)buf;
    de->inode = EXT2_ROOT_INO;
    de->rec_len = EXT2_DIR_REC_LEN(1);
    de->name_len = 1;
    de->file_type = EXT2_FT_DIR;
    de->name[0] = '.';

    de = (ext2_dirent_t *)(buf + EXT2_DIR_REC_LEN(1));
    de->inode = EXT2_ROOT_INO;
    de->rec_len = EXT2_DIR_REC_LEN(1);
    de->name_len = 2;
    de->file_type = EXT2_FT_DIR;
    de->name[0] = '.';
    de->name[1] = '.';

    de = (ext2_dirent_t *)(buf + 2 * EXT2_DIR_REC_LEN(1));
    de->inode = EXT2_GOOD_OLD_FIRST_INO;
    de->rec_len = EXT2_DIR_REC_LEN(10);
    de->name_len = 10;
    de->file_type = EXT2_FT_DIR;
    memcpy(de->name, "lost+found", 10);

    de = (ext2_dirent_t *)(buf + 2 * EXT2_DIR_REC_LEN(1) + EXT2_DIR_REC_LEN(10));
    de->inode = 0;
    de->rec_len = (uint16_t)(bs - 2 * EXT2_DIR_REC_LEN(1) - EXT2_DIR_REC_LEN(10));

    if (dev->write_sectors(dev, (uint64_t)root_block * spb, spb, buf) != 0) { kfree(buf); return FS_ERR_IO; }

    memset(buf, 0, bs);
    de = (ext2_dirent_t *)buf;
    de->inode = EXT2_GOOD_OLD_FIRST_INO;
    de->rec_len = EXT2_DIR_REC_LEN(1);
    de->name_len = 1;
    de->file_type = EXT2_FT_DIR;
    de->name[0] = '.';

    de = (ext2_dirent_t *)(buf + EXT2_DIR_REC_LEN(1));
    de->inode = EXT2_ROOT_INO;
    de->rec_len = (uint16_t)(bs - EXT2_DIR_REC_LEN(1));
    de->name_len = 2;
    de->file_type = EXT2_FT_DIR;
    de->name[0] = '.';
    de->name[1] = '.';

    if (dev->write_sectors(dev, (uint64_t)lf_block * spb, spb, buf) != 0) { kfree(buf); return FS_ERR_IO; }

    ext2_inode_t ino;
    memset(&ino, 0, sizeof(ino));
    ino.i_mode = EXT2_S_IFDIR | 0755;
    ino.i_links_count = 3;
    inode_set_size(&ino, bs);
    ino.i_blocks_lo = bs / 512u;
    ino.i_block[0] = root_block;
    r = fmt_write_inode(dev, ss, bs, meta0 + 2, inode_size, EXT2_ROOT_INO, &ino);
    if (r != FS_OK) { kfree(buf); return r; }

    memset(&ino, 0, sizeof(ino));
    ino.i_mode = EXT2_S_IFDIR | 0755;
    ino.i_links_count = 2;
    inode_set_size(&ino, bs);
    ino.i_blocks_lo = bs / 512u;
    ino.i_block[0] = lf_block;
    r = fmt_write_inode(dev, ss, bs, meta0 + 2, inode_size, EXT2_GOOD_OLD_FIRST_INO, &ino);
    if (r != FS_OK) { kfree(buf); return r; }

    kfree(buf);
    if (dev->flush) dev->flush(dev);
    return FS_OK;
}
