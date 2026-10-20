// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef PROC_INITFS_H
#define PROC_INITFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { INITFS_DIR = 1, INITFS_FILE = 2, INITFS_LINK = 3 };

void initfs_init(void);
bool initfs_present(void);

int initfs_lookup(const char *abs, bool follow, uint32_t *idx);

int initfs_type(uint32_t idx);
uint64_t initfs_size(uint32_t idx);
uint32_t initfs_mode(uint32_t idx);
uint64_t initfs_ino(uint32_t idx);
long initfs_readlink(uint32_t idx, char *buf, size_t cap);

const uint8_t *initfs_data(uint32_t idx);

bool initfs_next_child(uint32_t dir, uint32_t *cursor, char *name, size_t cap, int *type, uint32_t *child);
bool initfs_has_child(uint32_t dir, const char *name);

void initfs_stats(uint32_t *entries, uint64_t *image_bytes, uint64_t *file_bytes, uint64_t *cached_bytes);

#endif
