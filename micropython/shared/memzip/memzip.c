#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "py/mpconfig.h"
#include "py/misc.h"
#include "memzip.h"

extern uint8_t memzip_data[];
extern uint8_t memzip_data_end[] __attribute__((weak));

#define MEMZIP_FALLBACK_BLOB_LIMIT (8u * 1024u * 1024u)

static const uint8_t *memzip_blob_end(void) {
    if ((uintptr_t)memzip_data_end != 0) return memzip_data_end;
    return memzip_data + MEMZIP_FALLBACK_BLOB_LIMIT;
}

static int memzip_advance(const MEMZIP_FILE_HDR **file_hdr) {
    const uint8_t *p = (const uint8_t *)*file_hdr;
    const uint8_t *end = memzip_blob_end();
    if (p + sizeof(MEMZIP_FILE_HDR) > end) return 0;
    if ((*file_hdr)->signature != MEMZIP_FILE_HEADER_SIGNATURE) return 0;
    const uint8_t *q = p + sizeof(MEMZIP_FILE_HDR);
    uint32_t name_len = (*file_hdr)->filename_len;
    uint32_t extra_len = (*file_hdr)->extra_len;
    uint32_t data_len = (*file_hdr)->uncompressed_size;
    if (name_len > (uint32_t)(end - q)) return 0;
    q += name_len;
    if (extra_len > (uint32_t)(end - q)) return 0;
    q += extra_len;
    if (data_len > (uint32_t)(end - q)) return 0;
    q += data_len;
    *file_hdr = (const MEMZIP_FILE_HDR *)q;
    return 1;
}

const MEMZIP_FILE_HDR *memzip_find_file_header(const char *filename) {

    const MEMZIP_FILE_HDR *file_hdr = (const MEMZIP_FILE_HDR *)memzip_data;
    uint8_t *mem_data;

    /* Zip file filenames don't have a leading /, so we strip it off */

    if (*filename == '/') {
        filename++;
    }
    while (file_hdr->signature == MEMZIP_FILE_HEADER_SIGNATURE) {
        const char *file_hdr_filename = (const char *)&file_hdr[1];
        mem_data = (uint8_t *)file_hdr_filename;
        mem_data += file_hdr->filename_len;
        mem_data += file_hdr->extra_len;
        if (!strncmp(file_hdr_filename, filename, file_hdr->filename_len)) {
            /* We found a match */
            return file_hdr;
        }
        if (!memzip_advance(&file_hdr)) break;
    }
    return NULL;
}

bool memzip_is_dir(const char *filename) {
    const MEMZIP_FILE_HDR *file_hdr = (const MEMZIP_FILE_HDR *)memzip_data;
    uint8_t *mem_data;

    if (strcmp(filename, "/") == 0) {
        // The root directory is a directory.
        return true;
    }

    // Zip filenames don't have a leading /, so we strip it off
    if (*filename == '/') {
        filename++;
    }
    size_t filename_len = strlen(filename);

    while (file_hdr->signature == MEMZIP_FILE_HEADER_SIGNATURE) {
        const char *file_hdr_filename = (const char *)&file_hdr[1];
        if (filename_len < file_hdr->filename_len &&
            strncmp(file_hdr_filename, filename, filename_len) == 0 &&
            file_hdr_filename[filename_len] == '/') {
            return true;
        }

        if (!memzip_advance(&file_hdr)) break;
    }
    return false;

}

MEMZIP_RESULT memzip_locate(const char *filename, void **data, size_t *len) {
    const MEMZIP_FILE_HDR *file_hdr = memzip_find_file_header(filename);
    if (file_hdr == NULL) {
        return MZ_NO_FILE;
    }
    if (file_hdr->compression_method != 0) {
        return MZ_FILE_COMPRESSED;
    }

    uint8_t *mem_data;
    mem_data = (uint8_t *)&file_hdr[1];
    mem_data += file_hdr->filename_len;
    mem_data += file_hdr->extra_len;

    *data = mem_data;
    *len = file_hdr->uncompressed_size;
    return MZ_OK;
}

MEMZIP_RESULT memzip_stat(const char *path, MEMZIP_FILE_INFO *info) {
    const MEMZIP_FILE_HDR *file_hdr = memzip_find_file_header(path);
    if (file_hdr == NULL) {
        if (memzip_is_dir(path)) {
            info->file_size = 0;
            info->last_mod_date = 0;
            info->last_mod_time = 0;
            info->is_dir = 1;
            return MZ_OK;
        }
        return MZ_NO_FILE;
    }
    info->file_size = file_hdr->uncompressed_size;
    info->last_mod_date = file_hdr->last_mod_date;
    info->last_mod_time = file_hdr->last_mod_time;
    info->is_dir = 0;

    return MZ_OK;
}
