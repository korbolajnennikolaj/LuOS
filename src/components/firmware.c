// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "components/firmware.h"

#include "components/fwzip.h"
#include "components/logger.h"
#include "components/Memory/heap.h"
#include "fs/fs.h"
#include "kernel/programs/service_root_fs.h"
#include "kernel/proc/initfs.h"
#include "kernel/proc/process.h"
#include "kernel/scheduler/scheduler.h"
#include "kernel/scheduler/spinlock.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define FIRMWARE_CACHE_MAX 128

const char *const firmware_search_dirs[] = {
    "/lib/firmware",
    "/firmware",
    "/boot/firmware",
    NULL,
};

static struct firmware_blob *fw_cache[FIRMWARE_CACHE_MAX];
static spinlock_t fw_lock = SPINLOCK_INIT;

static struct {
    char path[FIRMWARE_PATH_MAX];
    enum firmware_source_kind kind;
    struct fwzip *zip;
    uint32_t entries;
} fw_src;

static volatile int fw_src_readers;

static const char *const firmware_zip_paths[] = {
    "/lib/firmware.zip",
    "/usr/lib/firmware.zip",
    "/firmware.zip",
    "/boot/firmware.zip",
    NULL,
};

static struct {
    struct kmutex lock;
    bool tried;
    struct fwzip *zip;
    const uint8_t *data;
    size_t size;
    char path[FIRMWARE_PATH_MAX];
} fw_img;

static const struct fwzip *fw_image_zip(void) {
    kmutex_lock(&fw_img.lock);
    if (!fw_img.tried) {
        fw_img.tried = true;
        for (int p = 0; firmware_zip_paths[p]; p++) {
            uint32_t idx;
            if (initfs_lookup(firmware_zip_paths[p], true, &idx) != 0) continue;
            if (initfs_type(idx) != INITFS_FILE) continue;
            const uint8_t *data = initfs_data(idx);
            if (!data) continue;
            struct fwzip *zip = NULL;
            int zr = fwzip_open_mem(data, initfs_size(idx), firmware_zip_paths[p], &zip);
            if (zr != FWZIP_OK) {
                LOG_WARNING("the image's firmware archive '%s' is not readable (%d)", firmware_zip_paths[p], zr);
                continue;
            }
            fw_img.zip = zip;
            fw_img.data = data;
            fw_img.size = (size_t)initfs_size(idx);
            strncpy(fw_img.path, firmware_zip_paths[p], sizeof(fw_img.path) - 1);
            fw_img.path[sizeof(fw_img.path) - 1] = '\0';
            LOG_INFO("built-in firmware: '%s' in the kernel image, %u entries, read on the fly, never unpacked",
                     fw_img.path, zip->count);
            break;
        }
    }
    struct fwzip *z = fw_img.zip;
    kmutex_unlock(&fw_img.lock);
    return z;
}

bool firmware_image_zip_present(void) {
    return fw_image_zip() != NULL;
}

const char *firmware_image_zip_path(void) {
    return fw_image_zip() ? fw_img.path : "";
}

uint32_t firmware_image_zip_entries(void) {
    const struct fwzip *z = fw_image_zip();
    return z ? z->count : 0;
}

static bool fw_name_ok(const char *name) {
    if (!name || !name[0] || name[0] == '/') return false;
    size_t n = strlen(name);
    if (n >= FIRMWARE_NAME_MAX) return false;
    for (size_t i = 0; i + 1 < n; i++)
        if (name[i] == '.' && name[i + 1] == '.') return false;
    return true;
}

static struct firmware_blob *fw_cache_lookup_locked(const char *name) {
    for (int i = 0; i < FIRMWARE_CACHE_MAX; i++)
        if (fw_cache[i] && strcmp(fw_cache[i]->name, name) == 0) return fw_cache[i];
    return NULL;
}

static int fw_read_file(fs_t *fs, const char *rel, uint8_t **data, size_t *size) {
    fs_file_t f;
    if (fs_open(fs, rel, false, false, &f) != FS_OK) return FIRMWARE_ERR_NOENT;

    uint64_t total = f.size;
    uint8_t *buf = kmalloc(total ? (size_t)total : 1);
    if (!buf) {
        fs_close(&f);
        return FIRMWARE_ERR_NOMEM;
    }

    uint64_t done = 0;
    while (done < total) {
        int64_t r = fs_read(&f, buf + done, total - done);
        if (r <= 0) break;
        done += (uint64_t)r;
    }
    fs_close(&f);

    if (done != total) {
        kfree(buf);
        return FIRMWARE_ERR_IO;
    }
    *data = buf;
    *size = (size_t)total;
    return FIRMWARE_OK;
}

static void fw_join(char *out, size_t cap, const char *a, const char *b, const char *c) {
    size_t n = 0;
    const char *parts[3] = { a, b, c };
    for (int p = 0; p < 3; p++) {
        for (const char *s = parts[p]; s && *s && n + 1 < cap; s++) {
            if (*s == '/' && n > 0 && out[n - 1] == '/') continue;
            out[n++] = *s;
        }
        if (p < 2 && parts[p + 1] && n + 1 < cap && (n == 0 || out[n - 1] != '/')) out[n++] = '/';
    }
    out[n] = '\0';
}

static int fw_publish(const char *name, const char *path, uint8_t *data, size_t size, bool builtin,
                      const struct firmware_blob **out) {
    struct firmware_blob *fw = kmalloc(sizeof(*fw));
    if (!fw) {
        if (!builtin) kfree(data);
        return FIRMWARE_ERR_NOMEM;
    }
    memset(fw, 0, sizeof(*fw));
    strncpy(fw->name, name, sizeof(fw->name) - 1);
    strncpy(fw->path, path, sizeof(fw->path) - 1);
    fw->data = data;
    fw->size = size;
    fw->refs = 1;
    fw->builtin = builtin;

    uint64_t flags = spin_lock_irqsave(&fw_lock);
    struct firmware_blob *race = fw_cache_lookup_locked(name);
    if (race) {
        race->refs++;
        spin_unlock_irqrestore(&fw_lock, flags);
        if (!builtin) kfree(data);
        kfree(fw);
        *out = race;
        return FIRMWARE_OK;
    }
    int slot = -1;
    for (int i = 0; i < FIRMWARE_CACHE_MAX; i++)
        if (!fw_cache[i]) { slot = i; break; }
    if (slot >= 0) fw_cache[slot] = fw;
    spin_unlock_irqrestore(&fw_lock, flags);

    if (slot < 0) LOG_WARNING("firmware cache full, '%s' is not shared", name);
    LOG_INFO("firmware '%s' loaded from %s%s (%llu bytes)", name, builtin ? "the kernel image, " : "", fw->path,
             (unsigned long long)size);
    *out = fw;
    return FIRMWARE_OK;
}

static struct {
    struct kmutex lock;
    uint64_t stamp;
    bool tried[ROOTFS_MAX_MOUNTS];
    struct fwzip *zip[ROOTFS_MAX_MOUNTS];
    char rel[ROOTFS_MAX_MOUNTS][FS_MAX_PATH];
    char path[ROOTFS_MAX_MOUNTS][FIRMWARE_PATH_MAX];
} fw_vol;

static volatile int fw_vol_readers;

static uint64_t fw_mounts_stamp(void) {
    uint64_t h = 14695981039346656037ULL;
    int n = rootfs_mount_count();
    h = (h ^ (uint64_t)(unsigned int)n) * 1099511628211ULL;
    for (int i = 0; i < n; i++) {
        const rootfs_mount_t *mnt = rootfs_mount_at(i);
        if (!mnt || !mnt->mounted) {
            h = (h ^ 0x5a) * 1099511628211ULL;
            continue;
        }
        for (const char *s = mnt->path; s && *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ULL;
        h = (h ^ 0x2f) * 1099511628211ULL;
        for (const char *s = mnt->device; s && *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ULL;
        h = (h ^ 0x2e) * 1099511628211ULL;
    }
    return h;
}

static const struct fwzip *fw_volume_acquire(int m, const rootfs_mount_t *mnt, char *rel, size_t rel_cap,
                                             char *path, size_t path_cap) {
    uint64_t stamp = fw_mounts_stamp();

    kmutex_lock(&fw_vol.lock);
    if (fw_vol.stamp != stamp) {
        struct fwzip *dead[ROOTFS_MAX_MOUNTS];
        int ndead = 0;
        for (int s = 0; s < ROOTFS_MAX_MOUNTS; s++) {
            if (fw_vol.zip[s]) dead[ndead++] = fw_vol.zip[s];
            fw_vol.zip[s] = NULL;
            fw_vol.tried[s] = false;
        }
        fw_vol.stamp = stamp;
        kmutex_unlock(&fw_vol.lock);
        while (fw_vol_readers > 0) scheduler_yield();
        for (int s = 0; s < ndead; s++) fwzip_close(dead[s]);
        kmutex_lock(&fw_vol.lock);
    }

    if (!fw_vol.tried[m]) {
        fw_vol.tried[m] = true;
        for (int p = 0; firmware_zip_paths[p]; p++) {
            const char *cand = firmware_zip_paths[p];
            fs_dirent_t st;
            if (fs_stat((fs_t *)&mnt->fs, cand, &st) != FS_OK || st.type != FS_ENTRY_FILE) continue;
            struct fwzip *z = NULL;
            int zr = fwzip_open((fs_t *)&mnt->fs, cand, &z);
            if (zr != FWZIP_OK) {
                LOG_WARNING("the firmware archive '%s' on volume '%s' is not readable (%d)", cand, mnt->path, zr);
                continue;
            }
            fw_vol.zip[m] = z;
            strncpy(fw_vol.rel[m], cand, sizeof(fw_vol.rel[m]) - 1);
            fw_vol.rel[m][sizeof(fw_vol.rel[m]) - 1] = '\0';
            fw_join(fw_vol.path[m], sizeof(fw_vol.path[m]), mnt->path, cand, NULL);
            LOG_INFO("firmware archive '%s' on volume '%s', %u entries, read on the fly, never unpacked",
                     fw_vol.path[m], mnt->path, z->count);
            break;
        }
    }

    const struct fwzip *z = fw_vol.zip[m];
    if (z) {
        if (rel && rel_cap) {
            strncpy(rel, fw_vol.rel[m], rel_cap - 1);
            rel[rel_cap - 1] = '\0';
        }
        if (path && path_cap) {
            strncpy(path, fw_vol.path[m], path_cap - 1);
            path[path_cap - 1] = '\0';
        }
        uint64_t flags = spin_lock_irqsave(&fw_lock);
        fw_vol_readers++;
        spin_unlock_irqrestore(&fw_lock, flags);
    }
    kmutex_unlock(&fw_vol.lock);
    return z;
}

static void fw_volume_release(void) {
    uint64_t flags = spin_lock_irqsave(&fw_lock);
    fw_vol_readers--;
    spin_unlock_irqrestore(&fw_lock, flags);
}

static int fw_request_volume(int m, const rootfs_mount_t *mnt, const char *name, int *loose_err,
                              const struct firmware_blob **out) {
    char rel[FS_MAX_PATH];
    char path[FIRMWARE_PATH_MAX];
    const struct fwzip *z = fw_volume_acquire(m, mnt, rel, sizeof(rel), path, sizeof(path));
    if (z) {
        const struct fwzip_entry *e = fwzip_find(z, name);
        if (e) {
            uint8_t *data = NULL;
            size_t size = 0;
            char desc[64];
            int zr = fwzip_read((fs_t *)&mnt->fs, rel, z, e, &data, &size, desc, sizeof(desc));
            char display[FIRMWARE_PATH_MAX];
            snprintf(display, sizeof(display), "%s:%s (%s, read on the fly)", path, e->name, desc);
            fw_volume_release();
            if (zr == FWZIP_OK) {
                return fw_publish(name, display, data, size, false, out);
            }
            LOG_WARNING("firmware '%s': reading the entry from '%s' failed (%d)", name, path, zr);
            return zr == FWZIP_ERR_NOMEM ? FIRMWARE_ERR_NOMEM
                : zr == FWZIP_ERR_PARAM ? FIRMWARE_ERR_PARAM : FIRMWARE_ERR_IO;
        }
        fw_volume_release();
    }

    for (int d = 0; firmware_search_dirs[d]; d++) {
        char rel2[FIRMWARE_PATH_MAX];
        fw_join(rel2, sizeof(rel2), firmware_search_dirs[d], name, NULL);

        uint8_t *data = NULL;
        size_t size = 0;
        int r = fw_read_file((fs_t *)&mnt->fs, rel2, &data, &size);
        if (r == FIRMWARE_ERR_NOENT) continue;
        if (r != FIRMWARE_OK) {
            *loose_err = r;
            continue;
        }

        char full[FIRMWARE_PATH_MAX];
        fw_join(full, sizeof(full), mnt->path, rel2, NULL);
        return fw_publish(name, full, data, size, false, out);
    }
    return FIRMWARE_ERR_NOENT;
}

void firmware_volume_zip_note(char *buf, size_t cap) {
    if (!buf || !cap) return;
    buf[0] = '\0';
    size_t used = 0;
    int mounts = rootfs_mount_count();
    for (int m = 0; m < mounts && m < ROOTFS_MAX_MOUNTS; m++) {
        const rootfs_mount_t *mnt = rootfs_mount_at(m);
        if (!mnt || !mnt->mounted) continue;
        char rel[FS_MAX_PATH];
        char path[FIRMWARE_PATH_MAX];
        const struct fwzip *z = fw_volume_acquire(m, mnt, rel, sizeof(rel), path, sizeof(path));
        if (!z) continue;
        uint32_t count = z->count;
        fw_volume_release();
        int n = snprintf(buf + used, cap - used, "%svolume archive %s, %u entries", used ? "; " : "", path,
                         count);
        if (n < 0 || (size_t)n >= cap - used) { buf[cap - 1] = '\0'; return; }
        used += (size_t)n;
    }
}

static int fw_request_builtin(const char *name, const struct firmware_blob **out) {
    if (!initfs_present()) return FIRMWARE_ERR_NOENT;

    const struct fwzip *iz = fw_image_zip();
    if (iz) {
        const struct fwzip_entry *e = fwzip_find(iz, name);
        if (e) {
            uint8_t *data = NULL;
            size_t size = 0;
            char desc[64];
            int zr = fwzip_read_mem(fw_img.data, fw_img.size, iz, e, &data, &size, desc, sizeof(desc));
            if (zr == FWZIP_OK) {
                char display[FIRMWARE_PATH_MAX];
                snprintf(display, sizeof(display), "%s:%s (%s, read on the fly from the image)", fw_img.path,
                         e->name, desc);
                return fw_publish(name, display, data, size, false, out);
            }
            LOG_WARNING("firmware '%s': reading the entry from the image archive failed (%d)", name, zr);
            return zr == FWZIP_ERR_NOMEM ? FIRMWARE_ERR_NOMEM
                : zr == FWZIP_ERR_PARAM ? FIRMWARE_ERR_PARAM : FIRMWARE_ERR_IO;
        }
    }

    for (int d = 0; firmware_search_dirs[d]; d++) {
        char abs[FIRMWARE_PATH_MAX];
        uint32_t idx;
        fw_join(abs, sizeof(abs), firmware_search_dirs[d], name, NULL);
        if (initfs_lookup(abs, true, &idx) != 0 || initfs_type(idx) != INITFS_FILE) continue;
        const uint8_t *data = initfs_data(idx);
        if (!data) return FIRMWARE_ERR_NOMEM;
        return fw_publish(name, abs, (uint8_t *)data, (size_t)initfs_size(idx), true, out);
    }
    return FIRMWARE_ERR_NOENT;
}

static int fw_request_source(const char *name, const struct firmware_blob **out) {
    struct {
        enum firmware_source_kind kind;
        char path[FIRMWARE_PATH_MAX];
        struct fwzip *zip;
    } snap;

    uint64_t flags = spin_lock_irqsave(&fw_lock);
    snap.kind = fw_src.kind;
    snap.zip = fw_src.zip;
    strncpy(snap.path, fw_src.path, sizeof(snap.path) - 1);
    snap.path[sizeof(snap.path) - 1] = '\0';
    bool active = snap.kind != FW_SOURCE_NONE;
    if (active && snap.kind == FW_SOURCE_ZIP) fw_src_readers++;
    spin_unlock_irqrestore(&fw_lock, flags);

    int r = FIRMWARE_ERR_NOENT;
    if (active) {
        char rel[FS_MAX_PATH];
        fs_t *fs = rootfs_resolve(snap.path, rel, sizeof(rel));
        if (!fs) {
            LOG_WARNING("firmware source '%s' is not on a mounted volume any more", snap.path);
            if (snap.kind == FW_SOURCE_ZIP) {
                uint64_t fl = spin_lock_irqsave(&fw_lock);
                fw_src_readers--;
                spin_unlock_irqrestore(&fw_lock, fl);
            }
            return FIRMWARE_ERR_NOENT;
        }

        if (snap.kind == FW_SOURCE_DIR) {
            static const char *const subdirs[] = { "", "firmware" };
            for (size_t d = 0; d < sizeof(subdirs) / sizeof(subdirs[0]) && r == FIRMWARE_ERR_NOENT; d++) {
                char rel2[FS_MAX_PATH];
                fw_join(rel2, sizeof(rel2), rel, subdirs[d], name);
                uint8_t *data = NULL;
                size_t size = 0;
                r = fw_read_file(fs, rel2, &data, &size);
                if (r == FIRMWARE_OK) {
                    char display[FIRMWARE_PATH_MAX];
                    if (subdirs[d][0]) snprintf(display, sizeof(display), "%s/%s/%s", snap.path, subdirs[d], name);
                    else snprintf(display, sizeof(display), "%s/%s", snap.path, name);
                    return fw_publish(name, display, data, size, false, out);
                }
            }
        } else {
            const struct fwzip_entry *e = fwzip_find(snap.zip, name);
            if (e) {
                uint8_t *data = NULL;
                size_t size = 0;
                char desc[64];
                int zr = fwzip_read(fs, rel, snap.zip, e, &data, &size, desc, sizeof(desc));
                if (zr == FWZIP_OK) {
                    char display[FIRMWARE_PATH_MAX];
                    snprintf(display, sizeof(display), "%s:%s (%s, read on the fly)", snap.path, e->name, desc);
                    r = fw_publish(name, display, data, size, false, out);
                } else {
                    r = zr == FWZIP_ERR_NOMEM ? FIRMWARE_ERR_NOMEM
                        : zr == FWZIP_ERR_PARAM ? FIRMWARE_ERR_PARAM : FIRMWARE_ERR_IO;
                }
            }
        }

        if (snap.kind == FW_SOURCE_ZIP) {
            uint64_t fl = spin_lock_irqsave(&fw_lock);
            fw_src_readers--;
            spin_unlock_irqrestore(&fw_lock, fl);
        }
    }

    if (r != FIRMWARE_ERR_NOENT && r != FIRMWARE_OK)
        LOG_WARNING("firmware '%s': the source '%s' failed (%d)", name, snap.path, r);
    return r;
}

static void fw_cache_drop(void) {
    for (int i = 0; i < FIRMWARE_CACHE_MAX; i++) {
        uint64_t flags = spin_lock_irqsave(&fw_lock);
        struct firmware_blob *fw = fw_cache[i];
        if (fw && fw->refs == 1) {
            fw_cache[i] = NULL;
            spin_unlock_irqrestore(&fw_lock, flags);
            if (!fw->builtin) kfree((void *)fw->data);
            kfree(fw);
        } else {
            spin_unlock_irqrestore(&fw_lock, flags);
        }
    }
}

int firmware_set_source(const char *path) {
    if (!path || !path[0]) {
        struct fwzip *old_zip;
        uint64_t flags = spin_lock_irqsave(&fw_lock);
        old_zip = fw_src.zip;
        fw_src.zip = NULL;
        fw_src.kind = FW_SOURCE_NONE;
        fw_src.entries = 0;
        fw_src.path[0] = '\0';
        spin_unlock_irqrestore(&fw_lock, flags);
        while (fw_src_readers > 0) scheduler_yield();
        if (old_zip) fwzip_close(old_zip);
        LOG_INFO("firmware source cleared, back to the built-in filesystem and /lib/firmware on the volumes");
        return FIRMWARE_OK;
    }

    char abs[FIRMWARE_PATH_MAX];
    rootfs_normalize_path(path, abs, sizeof(abs));

    char rel[FS_MAX_PATH];
    fs_t *fs = rootfs_resolve(abs, rel, sizeof(rel));
    if (!fs) {
        LOG_WARNING("firmware source '%s' is not on any mounted volume", abs);
        return FIRMWARE_ERR_NOENT;
    }

    fs_dirent_t st;
    int sr = fs_stat(fs, rel, &st);

    struct fwzip *zip = NULL;
    enum firmware_source_kind kind;
    uint32_t entries = 0;

    if (sr == FS_OK && st.type == FS_ENTRY_DIR) {
        kind = FW_SOURCE_DIR;
    } else if (sr == FS_OK && st.type == FS_ENTRY_FILE) {
        int zr = fwzip_open(fs, rel, &zip);
        if (zr != FWZIP_OK) {
            LOG_WARNING("'%s' is neither a firmware directory nor a readable zip archive (error %d)", abs, zr);
            return zr == FWZIP_ERR_NOMEM ? FIRMWARE_ERR_NOMEM : FIRMWARE_ERR_SOURCE;
        }
        kind = FW_SOURCE_ZIP;
        entries = zip->count;
    } else {
        LOG_WARNING("firmware source '%s' not found%s", abs, sr == FS_OK ? "" : " (stat failed)");
        return FIRMWARE_ERR_NOENT;
    }

    fw_cache_drop();

    struct fwzip *old_zip;
    uint64_t flags = spin_lock_irqsave(&fw_lock);
    old_zip = fw_src.zip;
    fw_src.kind = kind;
    strncpy(fw_src.path, abs, sizeof(fw_src.path) - 1);
    fw_src.path[sizeof(fw_src.path) - 1] = '\0';
    fw_src.zip = zip;
    fw_src.entries = entries;
    spin_unlock_irqrestore(&fw_lock, flags);

    while (fw_src_readers > 0) scheduler_yield();
    if (old_zip) fwzip_close(old_zip);

    if (kind == FW_SOURCE_DIR)
        LOG_INFO("firmware source: directory '%s' (its files and its firmware/ subtree)", abs);
    else
        LOG_INFO("firmware source: zip archive '%s', %u entries, read on the fly, never unpacked", abs, entries);
    return FIRMWARE_OK;
}

enum firmware_source_kind firmware_source_kind(void) {
    uint64_t flags = spin_lock_irqsave(&fw_lock);
    enum firmware_source_kind k = fw_src.kind;
    spin_unlock_irqrestore(&fw_lock, flags);
    return k;
}

const char *firmware_source_path(void) {
    uint64_t flags = spin_lock_irqsave(&fw_lock);
    const char *p = fw_src.path;
    spin_unlock_irqrestore(&fw_lock, flags);
    return p;
}

uint32_t firmware_source_entries(void) {
    uint64_t flags = spin_lock_irqsave(&fw_lock);
    uint32_t n = fw_src.entries;
    spin_unlock_irqrestore(&fw_lock, flags);
    return n;
}

int firmware_request(const char *name, const struct firmware_blob **out) {
    if (!out) return FIRMWARE_ERR_PARAM;
    *out = NULL;
    if (!fw_name_ok(name)) return FIRMWARE_ERR_PARAM;

    uint64_t flags = spin_lock_irqsave(&fw_lock);
    struct firmware_blob *hit = fw_cache_lookup_locked(name);
    if (hit) {
        hit->refs++;
        spin_unlock_irqrestore(&fw_lock, flags);
        *out = hit;
        return FIRMWARE_OK;
    }
    spin_unlock_irqrestore(&fw_lock, flags);

    int last_err = fw_request_source(name, out);
    if (last_err != FIRMWARE_ERR_NOENT) return last_err;

    last_err = fw_request_builtin(name, out);
    if (last_err != FIRMWARE_ERR_NOENT) return last_err;

    int loose_err = FIRMWARE_ERR_NOENT;
    int mounts = rootfs_mount_count();
    for (int m = 0; m < mounts && m < ROOTFS_MAX_MOUNTS; m++) {
        const rootfs_mount_t *mnt = rootfs_mount_at(m);
        if (!mnt || !mnt->mounted) continue;

        int r = fw_request_volume(m, mnt, name, &loose_err, out);
        if (r != FIRMWARE_ERR_NOENT) return r;
    }

    LOG_WARNING("firmware '%s' not found in the kernel image, its firmware.zip, or /lib/firmware, /firmware, "
                "/boot/firmware on %d mount(s)",
                name, mounts);
    return loose_err;
}


void firmware_release(const struct firmware_blob *cfw) {
    if (!cfw) return;
    struct firmware_blob *fw = (struct firmware_blob *)cfw;

    uint64_t flags = spin_lock_irqsave(&fw_lock);
    bool cached = false;
    int slot = -1;
    for (int i = 0; i < FIRMWARE_CACHE_MAX; i++)
        if (fw_cache[i] == fw) { cached = true; slot = i; break; }

    if (--fw->refs > 0) {
        spin_unlock_irqrestore(&fw_lock, flags);
        return;
    }
    if (cached) fw_cache[slot] = NULL;
    spin_unlock_irqrestore(&fw_lock, flags);

    if (!fw->builtin) kfree((void *)fw->data);
    kfree(fw);
}
