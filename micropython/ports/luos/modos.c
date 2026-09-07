// modos.c — the "os" module for MicroPython on LuOS.
//
// Wired directly to the kernel's single root filesystem mount (see
// kernel/rootfs.h) — the exact same mount the kernel shell's
// mount/ls/cd/cat commands use, not a separate Python-side filesystem.
// If nothing is mounted at "/", every function here raises
// OSError(ENODEV) — deliberately: an explicit "no filesystem" error is
// better than silently faking a filesystem or a successful no-op.
//
// os.uname(), meanwhile, is fully functional regardless of mount state
// — it's just static system information, no FS needed for it.

#include <string.h>
#include "py/obj.h"
#include "py/runtime.h"
#include "py/mperrno.h"
#include "kernel/kernel_info.h"

#include "fs/fs.h"
#include "kernel/rootfs.h"

// ----------------------------------------------------------------------
// FS_ERR_* -> MP_E* mapping. Shared with modfile.c (open()/file
// read/write/seek) via the forward declaration there, so both sides of
// the filesystem API (content in modfile.c, metadata here) raise
// consistent errno's for the same underlying condition.
// ----------------------------------------------------------------------

int rootfs_errno_for(int fs_err) {
    switch (fs_err) {
        case FS_OK:              return 0;
        case FS_ERR_NOENT:       return MP_ENOENT;
        case FS_ERR_EXIST:       return MP_EEXIST;
        case FS_ERR_NOTDIR:      return MP_ENOTDIR;
        case FS_ERR_ISDIR:       return MP_EISDIR;
        case FS_ERR_NOSPACE:     return MP_ENOSPC;
        case FS_ERR_PARAM:       return MP_EINVAL;
        case FS_ERR_NOSUPP:      return MP_EPERM;
        case FS_ERR_NOMEM:       return MP_ENOMEM;
        case FS_ERR_CORRUPT:     return MP_EIO;
        case FS_ERR_NOTMOUNTED:  return MP_ENODEV;
        case FS_ERR_NOTEMPTY:    return MP_EEXIST; // no MP_ENOTEMPTY in mperrno.h
        case FS_ERR_EOF:         return 0;
        case FS_ERR_IO:          return MP_EIO;
        default:                 return MP_EIO;
    }
}

// Every function below that touches the disk starts with this - raises
// immediately (and consistently) if nothing is mounted, instead of each
// function separately deciding what "no filesystem" should look like.
static fs_t *require_mounted(void) {
    if (!rootfs_is_mounted()) {
        mp_raise_OSError(MP_ENODEV);
    }
    return rootfs_get();
}

static void raise_fs_err(int fs_err) {
    mp_raise_OSError(rootfs_errno_for(fs_err));
}

// ----------------------------------------------------------------------
// os.uname() -> (sysname, nodename, release, version, machine)
// ----------------------------------------------------------------------

static mp_obj_t mod_os_uname(void) {
    mp_obj_t items[5] = {
        mp_obj_new_str("LuOS", 4),
        mp_obj_new_str("luos", 4),
        mp_obj_new_str(KERNEL_VERSION, strlen(KERNEL_VERSION)),
        mp_obj_new_str(MICROPY_VERSION_STRING, strlen(MICROPY_VERSION_STRING)),
        mp_obj_new_str("x86_64", 6),
    };
    return mp_obj_new_tuple(5, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_os_uname_obj, mod_os_uname);

// ----------------------------------------------------------------------
// os.getcwd() / os.chdir(path)
//
// Shares the exact same cwd as the kernel shell (rootfs_cwd()) - `cd`
// in the shell and os.chdir() in Python affect and observe the same
// current directory. LuOS is single-tasking, so there's just one cwd,
// full stop - same as how a single-user classic distro shell behaves.
// ----------------------------------------------------------------------

static mp_obj_t mod_os_getcwd(void) {
    const char *cwd = rootfs_cwd();
    return mp_obj_new_str(cwd, strlen(cwd));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_os_getcwd_obj, mod_os_getcwd);

static mp_obj_t mod_os_chdir(mp_obj_t path_obj) {
    fs_t *fs = require_mounted();
    const char *path_in = mp_obj_str_get_str(path_obj);

    char path[FS_MAX_PATH];
    rootfs_normalize_path(path_in, path, sizeof(path));

    fs_dirent_t st;
    int r = fs_stat(fs, path, &st);
    if (r != FS_OK) raise_fs_err(r);
    if (st.type != FS_ENTRY_DIR) mp_raise_OSError(MP_ENOTDIR);

    rootfs_set_cwd(path);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_os_chdir_obj, mod_os_chdir);

// ----------------------------------------------------------------------
// os.listdir([path='.']) -> [name, ...]
// ----------------------------------------------------------------------

static mp_obj_t mod_os_listdir(size_t n_args, const mp_obj_t *args) {
    fs_t *fs = require_mounted();
    const char *path_in = (n_args > 0) ? mp_obj_str_get_str(args[0]) : ".";

    char path[FS_MAX_PATH];
    rootfs_normalize_path(path_in, path, sizeof(path));

    fs_dir_t dir;
    int r = fs_opendir(fs, path, &dir);
    if (r != FS_OK) raise_fs_err(r);

    mp_obj_t list = mp_obj_new_list(0, NULL);
    fs_dirent_t ent;
    while (fs_readdir(&dir, &ent) == FS_OK) {
        mp_obj_list_append(list, mp_obj_new_str(ent.name, strlen(ent.name)));
    }
    fs_closedir(&dir);
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_os_listdir_obj, 0, 1, mod_os_listdir);

// ----------------------------------------------------------------------
// os.stat(path) -> 10-tuple, same shape as CPython's/other MicroPython
// ports' os.stat: (mode, ino, dev, nlink, uid, gid, size, atime, mtime,
// ctime). This kernel's FS layer doesn't track most of those, so only
// mode (dir-bit + fixed permission bits) and size are meaningful; the
// rest are 0 - scripts that only check stat(...).st_size or
// stat.S_ISDIR(stat(...).st_mode) (the common cases) work correctly.
// ----------------------------------------------------------------------

#define LUOS_S_IFDIR 0x4000
#define LUOS_S_IFREG 0x8000

static mp_obj_t mod_os_stat(mp_obj_t path_obj) {
    fs_t *fs = require_mounted();
    const char *path_in = mp_obj_str_get_str(path_obj);

    char path[FS_MAX_PATH];
    rootfs_normalize_path(path_in, path, sizeof(path));

    fs_dirent_t st;
    int r = fs_stat(fs, path, &st);
    if (r != FS_OK) raise_fs_err(r);

    mp_int_t mode = (st.type == FS_ENTRY_DIR) ? (LUOS_S_IFDIR | 0755) : (LUOS_S_IFREG | 0644);

    mp_obj_t items[10] = {
        mp_obj_new_int(mode),
        mp_obj_new_int(0),  // ino
        mp_obj_new_int(0),  // dev
        mp_obj_new_int(1),  // nlink
        mp_obj_new_int(0),  // uid
        mp_obj_new_int(0),  // gid
        mp_obj_new_int_from_uint(st.size),
        mp_obj_new_int(0),  // atime
        mp_obj_new_int(0),  // mtime
        mp_obj_new_int(0),  // ctime
    };
    return mp_obj_new_tuple(10, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_os_stat_obj, mod_os_stat);

// ----------------------------------------------------------------------
// os.remove(path) / os.rmdir(path) / os.mkdir(path)
// ----------------------------------------------------------------------

static mp_obj_t mod_os_remove(mp_obj_t path_obj) {
    fs_t *fs = require_mounted();
    const char *path_in = mp_obj_str_get_str(path_obj);

    char path[FS_MAX_PATH];
    rootfs_normalize_path(path_in, path, sizeof(path));

    int r = fs_unlink(fs, path);
    if (r != FS_OK) raise_fs_err(r);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_os_remove_obj, mod_os_remove);

static mp_obj_t mod_os_mkdir(mp_obj_t path_obj) {
    fs_t *fs = require_mounted();
    const char *path_in = mp_obj_str_get_str(path_obj);

    char path[FS_MAX_PATH];
    rootfs_normalize_path(path_in, path, sizeof(path));

    int r = fs_mkdir(fs, path);
    if (r != FS_OK) raise_fs_err(r);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_os_mkdir_obj, mod_os_mkdir);

static mp_obj_t mod_os_rmdir(mp_obj_t path_obj) {
    fs_t *fs = require_mounted();
    const char *path_in = mp_obj_str_get_str(path_obj);

    char path[FS_MAX_PATH];
    rootfs_normalize_path(path_in, path, sizeof(path));

    int r = fs_rmdir(fs, path);
    if (r != FS_OK) raise_fs_err(r);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_os_rmdir_obj, mod_os_rmdir);

// ----------------------------------------------------------------------
// Module registration
// ----------------------------------------------------------------------

static const mp_rom_map_elem_t os_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_os) },

    { MP_ROM_QSTR(MP_QSTR_uname), MP_ROM_PTR(&mod_os_uname_obj) },
    { MP_ROM_QSTR(MP_QSTR_getcwd), MP_ROM_PTR(&mod_os_getcwd_obj) },
    { MP_ROM_QSTR(MP_QSTR_chdir), MP_ROM_PTR(&mod_os_chdir_obj) },

    { MP_ROM_QSTR(MP_QSTR_listdir), MP_ROM_PTR(&mod_os_listdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_stat), MP_ROM_PTR(&mod_os_stat_obj) },
    { MP_ROM_QSTR(MP_QSTR_remove), MP_ROM_PTR(&mod_os_remove_obj) },
    { MP_ROM_QSTR(MP_QSTR_mkdir), MP_ROM_PTR(&mod_os_mkdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_rmdir), MP_ROM_PTR(&mod_os_rmdir_obj) },
};
static MP_DEFINE_CONST_DICT(os_module_globals, os_module_globals_table);

const mp_obj_module_t mp_module_os = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&os_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_os, mp_module_os);
