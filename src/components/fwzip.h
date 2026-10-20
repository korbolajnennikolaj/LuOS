// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef FWZIP_H
#define FWZIP_H


#include "fs/fs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FWZIP_NAME_MAX 128
#define FWZIP_MAX_ENTRIES 65536

#define FWZIP_OK 0
#define FWZIP_ERR_PARAM -1
#define FWZIP_ERR_NOENT -2
#define FWZIP_ERR_NOMEM -3
#define FWZIP_ERR_IO -4
#define FWZIP_ERR_CORRUPT -5
#define FWZIP_ERR_UNSUPP -6

struct fwzip_entry {
    char name[FWZIP_NAME_MAX];
    uint64_t header_offset;
    uint32_t comp_size;
    uint32_t file_size;
    uint32_t crc32;
    uint16_t method;
};

struct fwzip {
    struct fwzip_entry *entries;
    uint32_t count;
};

int fwzip_open(fs_t *fs, const char *rel, struct fwzip **out);

int fwzip_open_mem(const uint8_t *mem, uint64_t mem_len, const char *label, struct fwzip **out);

void fwzip_close(struct fwzip *z);

const struct fwzip_entry *fwzip_find(const struct fwzip *z, const char *name);

int fwzip_read(fs_t *fs, const char *rel, const struct fwzip *z, const struct fwzip_entry *e,
               uint8_t **data, size_t *size, char *out_desc, size_t desc_cap);

int fwzip_read_mem(const uint8_t *mem, uint64_t mem_len, const struct fwzip *z, const struct fwzip_entry *e,
                   uint8_t **data, size_t *size, char *out_desc, size_t desc_cap);

int fwzip_inflate(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len);

uint32_t fwzip_crc32(uint32_t crc, const uint8_t *p, size_t len);

#endif
