#ifndef LUOS_EXT2_H
#define LUOS_EXT2_H

#include "drivers/Storage/block_device.h"
#include "fs/fs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EXT2_SUPER_MAGIC 0xEF53u
#define EXT2_SUPERBLOCK_OFFSET 1024u
#define EXT2_ROOT_INO 2u
#define EXT2_GOOD_OLD_FIRST_INO 11u
#define EXT2_GOOD_OLD_INODE_SIZE 128u

#define EXT2_FEATURE_COMPAT_DIR_PREALLOC 0x0001u
#define EXT2_FEATURE_COMPAT_IMAGIC_INODES 0x0002u
#define EXT2_FEATURE_COMPAT_HAS_JOURNAL 0x0004u
#define EXT2_FEATURE_COMPAT_EXT_ATTR 0x0008u
#define EXT2_FEATURE_COMPAT_RESIZE_INO 0x0010u
#define EXT2_FEATURE_COMPAT_DIR_INDEX 0x0020u

#define EXT2_FEATURE_INCOMPAT_COMPRESSION 0x0001u
#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002u
#define EXT2_FEATURE_INCOMPAT_META_BG 0x0010u
#define EXT2_FEATURE_INCOMPAT_EXTENTS 0x0040u
#define EXT2_FEATURE_INCOMPAT_64BIT 0x0080u
#define EXT2_FEATURE_INCOMPAT_MMP 0x1000u
#define EXT3_FEATURE_INCOMPAT_RECOVER 0x0004u
#define EXT3_FEATURE_INCOMPAT_JOURNAL_DEV 0x0008u

#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001u
#define EXT2_FEATURE_RO_COMPAT_LARGE_FILE 0x0002u
#define EXT2_FEATURE_RO_COMPAT_BTREE_DIR 0x0004u
#define EXT2_FEATURE_RO_COMPAT_HUGE_FILE 0x0008u

#define EXT2_NDIR_BLOCKS 12
#define EXT2_IND_BLOCK EXT2_NDIR_BLOCKS
#define EXT2_DIND_BLOCK (EXT2_NDIR_BLOCKS + 1)
#define EXT2_TIND_BLOCK (EXT2_NDIR_BLOCKS + 2)
#define EXT2_N_BLOCKS 15

#define EXT2_S_IFMT 0xF000u
#define EXT2_S_IFLNK 0xA000u
#define EXT2_S_IFREG 0x8000u
#define EXT2_S_IFDIR 0x4000u

#define EXT2_FT_UNKNOWN 0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR 2
#define EXT2_FT_CHRDEV 3
#define EXT2_FT_BLKDEV 4
#define EXT2_FT_FIFO 5
#define EXT2_FT_SOCK 6
#define EXT2_FT_SYMLINK 7

typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_cluster_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t s_uuid[16];
    char s_volume_name[16];
    char s_last_mounted[64];
    uint32_t s_algo_bitmap;
    uint8_t s_prealloc_blocks;
    uint8_t s_prealloc_dir_blocks;
    uint16_t s_reserved_gdt_blocks;
    uint8_t s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t s_def_hash_version;
    uint8_t s_jnl_backup_type;
    uint16_t s_desc_size;
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;
    uint32_t s_mkfs_time;
    uint32_t s_jnl_blocks[17];
} ext2_superblock_t;

typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t bg_reserved[12];
} ext2_group_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size_lo;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks_lo;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[EXT2_N_BLOCKS];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_size_high;
} ext2_inode_t;

typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
    char name[];
} ext2_dirent_t;

typedef struct ext2_fs {
    struct block_device *dev;
    ext2_superblock_t sb;
    uint32_t block_size;
    uint32_t sectors_per_block;
    uint32_t first_data_block;
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t inode_size;
    uint32_t num_groups;
    uint32_t desc_size;
    uint64_t blocks_count;
    bool has_filetype;
    bool has_journal;
    bool dirty;
    uint8_t *scratch;
} ext2_fs_t;

typedef struct ext2_file {
    ext2_fs_t *fs;
    uint32_t ino;
    ext2_inode_t inode;
    uint64_t size;
    bool writable;
} ext2_file_t;

typedef struct ext2_diriter {
    ext2_fs_t *fs;
    uint32_t ino;
    ext2_inode_t inode;
    uint64_t pos;
    uint64_t size;
    uint8_t *blockbuf;
    uint32_t loaded_lblk;
} ext2_diriter_t;

int ext2_probe(struct block_device *dev);
int ext2_is_ext3(struct block_device *dev);
int ext2_mount(struct block_device *dev, fs_t *out);
int ext2_format(struct block_device *dev, const char *label, int is_ext3);

extern const fs_ops_t ext2_ops;

#endif
