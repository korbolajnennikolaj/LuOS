#include "service_root_fs.h"

#include "components/drivers.h"
#include "drivers/Storage/block_device.h"
#include "kernel/scheduler/scheduler.h"
#include "kernel/scheduler/spinlock.h"

#include <stdio.h>
#include <string.h>

#define ROOT_FS_TICK_MS 1000
#define ROOT_FS_AUTO_MOUNT_TICKS 5
#define ROOT_FS_WATCHDOG_MS 6000

static rootfs_mount_t mounts[ROOTFS_MAX_MOUNTS];
static bool mount_pending[ROOTFS_MAX_MOUNTS];
static spinlock_t rootfs_lock = SPINLOCK_INIT;

static char g_cwd[FS_MAX_PATH] = "/";

static bool auto_mount_allowed = true;

static volatile uint64_t root_fs_last_beat_ms = 0;

static void fs_normalize_path(const char *base_cwd, const char *input, char *out, size_t out_cap) {
    char combined[FS_MAX_PATH];
    if (input[0] == '/') {
        strncpy(combined, input, sizeof(combined) - 1);
    } else {
        snprintf(combined, sizeof(combined), "%s/%s", base_cwd, input);
    }
    combined[sizeof(combined) - 1] = '\0';

    char *stack[64];
    int depth = 0;

    char *p = combined;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *start = p;
        while (*p && *p != '/') p++;
        if (*p) { *p = '\0'; p++; }

        if (strcmp(start, ".") == 0) {

        } else if (strcmp(start, "..") == 0) {
            if (depth > 0) depth--;
        } else if (depth < 64) {
            stack[depth++] = start;
        }
    }

    if (depth == 0) {
        strncpy(out, "/", out_cap - 1);
        out[out_cap - 1] = '\0';
        return;
    }

    size_t pos = 0;
    for (int i = 0; i < depth; i++) {
        size_t l = strlen(stack[i]);
        if (pos + 1 + l >= out_cap) break;
        out[pos++] = '/';
        memcpy(out + pos, stack[i], l);
        pos += l;
    }
    out[pos] = '\0';
}

static bool path_under_mount(const char *mount_path, const char *abs_path) {
    size_t len = strlen(mount_path);
    if (len == 1 && mount_path[0] == '/') return true;
    if (strncmp(abs_path, mount_path, len) != 0) return false;
    return abs_path[len] == '\0' || abs_path[len] == '/';
}

static int find_mount_slot(const char *mount_point) {
    for (int i = 0; i < ROOTFS_MAX_MOUNTS; i++) {
        if (mounts[i].mounted && strcmp(mounts[i].path, mount_point) == 0) return i;
    }
    return -1;
}

static int find_free_slot(const char *mount_point) {
    if (strcmp(mount_point, "/") == 0) {
        if (mounts[0].mounted || mount_pending[0]) return -1;
        return 0;
    }
    for (int i = 1; i < ROOTFS_MAX_MOUNTS; i++) {
        if (!mounts[i].mounted && !mount_pending[i]) return i;
    }
    return -1;
}

int rootfs_is_mounted(void) {
    return mounts[0].mounted ? 1 : 0;
}

fs_t *rootfs_get(void) {
    return mounts[0].mounted ? &mounts[0].fs : NULL;
}

const char *rootfs_cwd(void) {
    return g_cwd;
}

void rootfs_set_cwd(const char *path) {
    strncpy(g_cwd, path, sizeof(g_cwd) - 1);
    g_cwd[sizeof(g_cwd) - 1] = '\0';
}

void rootfs_normalize_path(const char *input, char *out, size_t out_cap) {
    fs_normalize_path(g_cwd, (input && input[0]) ? input : ".", out, out_cap);
}

const char *rootfs_type_name(enum fs_type type) {
    switch (type) {
        case FS_TYPE_FAT32:   return "FAT32";
        case FS_TYPE_EXFAT:   return "exFAT";
        case FS_TYPE_EXT4:    return "ext4";
        case FS_TYPE_ISO9660: return "ISO9660";
        default:              return "unknown";
    }
}

const char *rootfs_root_device(void) {
    return mounts[0].mounted ? mounts[0].device : "";
}

int rootfs_mount_count(void) {
    int count = 0;
    for (int i = 0; i < ROOTFS_MAX_MOUNTS; i++)
        if (mounts[i].mounted) count++;
    return count;
}

const rootfs_mount_t *rootfs_mount_at(int index) {
    int seen = 0;
    for (int i = 0; i < ROOTFS_MAX_MOUNTS; i++) {
        if (!mounts[i].mounted) continue;
        if (seen == index) return &mounts[i];
        seen++;
    }
    return NULL;
}

const rootfs_mount_t *rootfs_mount_for_path(const char *abs_path) {
    if (!abs_path || abs_path[0] != '/') return NULL;

    rootfs_mount_t *best = NULL;
    size_t best_len = 0;

    for (int i = 0; i < ROOTFS_MAX_MOUNTS; i++) {
        if (!mounts[i].mounted) continue;
        if (!path_under_mount(mounts[i].path, abs_path)) continue;

        size_t len = strlen(mounts[i].path);
        if (!best || len > best_len) {
            best = &mounts[i];
            best_len = len;
        }
    }
    return best;
}

fs_t *rootfs_resolve(const char *abs_path, char *out_rel, size_t out_cap) {
    const rootfs_mount_t *mnt = rootfs_mount_for_path(abs_path);
    if (!mnt) return NULL;

    if (out_rel && out_cap > 0) {
        size_t len = strlen(mnt->path);
        const char *rel = (len == 1) ? abs_path : abs_path + len;
        if (rel[0] == '\0') rel = "/";
        strncpy(out_rel, rel, out_cap - 1);
        out_rel[out_cap - 1] = '\0';
    }
    return (fs_t *)&mnt->fs;
}

static int rootfs_prepare_point(const char *mount_point) {
    if (strcmp(mount_point, "/") == 0) return ROOTFS_OK;
    if (!mounts[0].mounted) return ROOTFS_ERR_NOROOT;

    char rel[FS_MAX_PATH];
    fs_t *fs = rootfs_resolve(mount_point, rel, sizeof(rel));
    if (!fs) return ROOTFS_ERR_NOROOT;

    fs_dirent_t st;
    int r = fs_stat(fs, rel, &st);
    if (r == FS_OK) return (st.type == FS_ENTRY_DIR) ? ROOTFS_OK : ROOTFS_ERR_PARAM;

    if (fs_mkdir(fs, rel) != FS_OK) return ROOTFS_ERR_NOENT;
    return ROOTFS_OK;
}

int rootfs_mount_device(const char *device_name, const char *mount_point, int *out_fs_err) {
    if (out_fs_err) *out_fs_err = FS_OK;
    if (!device_name || !device_name[0] || !mount_point || mount_point[0] != '/') return ROOTFS_ERR_PARAM;
    if (strlen(mount_point) >= ROOTFS_MOUNT_PATH_MAX) return ROOTFS_ERR_PARAM;
    if (strlen(device_name) >= ROOTFS_DEVICE_NAME_MAX) return ROOTFS_ERR_PARAM;

    char point[ROOTFS_MOUNT_PATH_MAX];
    fs_normalize_path("/", mount_point, point, sizeof(point));

    uint32_t idx = get_disk_index_from_name(device_name);
    if (idx == UINT32_MAX) return ROOTFS_ERR_NODEV;

    struct block_device *dev = block_device_get(idx);
    if (!dev) return ROOTFS_ERR_NODEV;

    spin_lock(&rootfs_lock);

    if (find_mount_slot(point) >= 0) {
        spin_unlock(&rootfs_lock);
        return ROOTFS_ERR_BUSY;
    }

    for (int i = 0; i < ROOTFS_MAX_MOUNTS; i++) {
        if (mounts[i].mounted && strcmp(mounts[i].device, device_name) == 0) {
            spin_unlock(&rootfs_lock);
            return ROOTFS_ERR_BUSY;
        }
    }

    int slot = find_free_slot(point);
    if (slot < 0) {
        spin_unlock(&rootfs_lock);
        return (strcmp(point, "/") == 0) ? ROOTFS_ERR_BUSY : ROOTFS_ERR_NOSPACE;
    }

    mount_pending[slot] = true;
    spin_unlock(&rootfs_lock);

    int prep = rootfs_prepare_point(point);
    if (prep != ROOTFS_OK) {
        mount_pending[slot] = false;
        return prep;
    }

    fs_t fs;
    int r = fs_mount_auto(dev, &fs);
    if (r != FS_OK) {
        mount_pending[slot] = false;
        if (out_fs_err) *out_fs_err = r;
        return ROOTFS_ERR_FS;
    }

    spin_lock(&rootfs_lock);
    mounts[slot].fs = fs;
    strncpy(mounts[slot].path, point, ROOTFS_MOUNT_PATH_MAX - 1);
    mounts[slot].path[ROOTFS_MOUNT_PATH_MAX - 1] = '\0';
    strncpy(mounts[slot].device, device_name, ROOTFS_DEVICE_NAME_MAX - 1);
    mounts[slot].device[ROOTFS_DEVICE_NAME_MAX - 1] = '\0';
    mounts[slot].mounted = true;
    mount_pending[slot] = false;
    spin_unlock(&rootfs_lock);

    if (slot == 0) rootfs_set_cwd("/");
    return ROOTFS_OK;
}

int rootfs_unmount_point(const char *mount_point) {
    if (!mount_point || mount_point[0] != '/') return ROOTFS_ERR_PARAM;

    char point[ROOTFS_MOUNT_PATH_MAX];
    fs_normalize_path("/", mount_point, point, sizeof(point));

    spin_lock(&rootfs_lock);

    int slot = find_mount_slot(point);
    if (slot < 0) {
        spin_unlock(&rootfs_lock);
        return ROOTFS_ERR_NOENT;
    }

    if (slot == 0) {
        for (int i = 1; i < ROOTFS_MAX_MOUNTS; i++) {
            if (mounts[i].mounted) {
                spin_unlock(&rootfs_lock);
                return ROOTFS_ERR_BUSY;
            }
        }
    }

    mounts[slot].mounted = false;
    mount_pending[slot] = true;
    spin_unlock(&rootfs_lock);

    fs_unmount(&mounts[slot].fs);

    spin_lock(&rootfs_lock);
    mounts[slot].device[0] = '\0';
    mounts[slot].path[0] = '\0';
    mount_pending[slot] = false;
    spin_unlock(&rootfs_lock);

    if (slot == 0) {
        auto_mount_allowed = false;
        rootfs_set_cwd("/");
    }
    return ROOTFS_OK;
}

static void rootfs_drop_lost_mounts(void) {
    for (int i = 0; i < ROOTFS_MAX_MOUNTS; i++) {
        if (!mounts[i].mounted || mount_pending[i]) continue;

        uint32_t idx = get_disk_index_from_name(mounts[i].device);
        struct block_device *dev = (idx == UINT32_MAX) ? NULL : block_device_get(idx);
        if (dev && dev == mounts[i].fs.dev) continue;

        spin_lock(&rootfs_lock);
        mounts[i].mounted = false;
        mounts[i].device[0] = '\0';
        mounts[i].path[0] = '\0';
        spin_unlock(&rootfs_lock);

        if (i == 0) rootfs_set_cwd("/");
    }
}

static void rootfs_try_auto_mount(void) {
    if (!auto_mount_allowed || mounts[0].mounted) return;

    uint32_t count = get_device_count();
    for (uint32_t i = 0; i < count; i++) {
        struct block_device *dev = block_device_get(i);
        if (!dev) continue;

        bool taken = false;
        for (int m = 1; m < ROOTFS_MAX_MOUNTS; m++) {
            if (mounts[m].mounted && mounts[m].fs.dev == dev) { taken = true; break; }
        }
        if (taken) continue;

        if (rootfs_mount_device(dev->name, "/", NULL) == ROOTFS_OK) return;
    }
}

static void root_fs_entry(void *arg) {
    (void)arg;

    service_t *svc = get_root_fs_service();
    uint32_t tick = 0;

    root_fs_last_beat_ms = service_uptime_ms();

    while (!service_stop_requested(svc->id)) {
        rootfs_drop_lost_mounts();

        if ((tick % ROOT_FS_AUTO_MOUNT_TICKS) == 0) rootfs_try_auto_mount();

        tick++;
        root_fs_last_beat_ms = service_uptime_ms();
        scheduler_sleep_ms(ROOT_FS_TICK_MS);
    }
}

static bool root_fs_update(void *arg) {
    (void)arg;
    if (root_fs_last_beat_ms == 0) return true;
    return (service_uptime_ms() - root_fs_last_beat_ms) < ROOT_FS_WATCHDOG_MS;
}

service_t *get_root_fs_service(void) {
    static service_t root_fs_service = {
        .name = "root_fs",
        .entry = root_fs_entry,
        .arg = NULL,
        .update = root_fs_update,
        .priority = SERVICE_HIGH_PRIORITY,
        .state = SERVICE_STATE_STOPPED,
        .dependency_count = 0,
        .restart_limit = SERVICE_RESTART_LIMIT_DEFAULT,
        .restart_count = 0,
        .task = NULL
    };

    return &root_fs_service;
}
