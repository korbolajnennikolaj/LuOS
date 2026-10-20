// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef FIRMWARE_H
#define FIRMWARE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FIRMWARE_NAME_MAX 128
#define FIRMWARE_PATH_MAX 256

struct firmware_blob {
    char name[FIRMWARE_NAME_MAX];
    char path[FIRMWARE_PATH_MAX];
    const uint8_t *data;
    size_t size;
    int refs;
    bool builtin;
};

#define FIRMWARE_OK 0
#define FIRMWARE_ERR_PARAM -1
#define FIRMWARE_ERR_NOENT -2
#define FIRMWARE_ERR_NOMEM -3
#define FIRMWARE_ERR_IO -4
#define FIRMWARE_ERR_SOURCE -5

int firmware_request(const char *name, const struct firmware_blob **out);
void firmware_release(const struct firmware_blob *fw);

int firmware_set_source(const char *path);
enum firmware_source_kind {
    FW_SOURCE_NONE = 0,
    FW_SOURCE_DIR,
    FW_SOURCE_ZIP,
};
enum firmware_source_kind firmware_source_kind(void);
const char *firmware_source_path(void);
uint32_t firmware_source_entries(void);

bool firmware_image_zip_present(void);
const char *firmware_image_zip_path(void);
uint32_t firmware_image_zip_entries(void);

void firmware_volume_zip_note(char *buf, size_t cap);

extern const char *const firmware_search_dirs[];

#endif
