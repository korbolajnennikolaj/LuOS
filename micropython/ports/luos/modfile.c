// modfile.c — the built-in `file` object type for MicroPython on LuOS,
// and the global open() function that creates it.
//
// This is deliberately NOT a VFS/mount abstraction of its own. There is
// exactly one filesystem in LuOS, mounted at "/" by the kernel shell's
// `mount` command (see rootfs.h) — the same one `cat`/`ls` use. Python's
// open("/some/path") reads and writes through that same mount, with the
// same current working directory for relative paths, exactly the way a
// normal OS has one shared root filesystem instead of every process
// mounting its own. If nothing is mounted, open() raises OSError(ENODEV)
// rather than pretending a filesystem exists.
//
// File objects implement MicroPython's generic stream protocol
// (py/stream.h), which gives read()/readline()/readlines()/write()/
// iteration/seek()/tell()/close()/context-manager support for free —
// this file only needs to provide the low-level read/write/ioctl
// callbacks that talk to fs_t.

#include <string.h>
#include "py/obj.h"
#include "py/runtime.h"
#include "py/mperrno.h"
#include "py/stream.h"
#include "py/builtin.h"

#include "fs/fs.h"
#include "kernel/rootfs.h"

// ----------------------------------------------------------------------
// FS_ERR_* -> MP_E* mapping, shared with modos.c (see rootfs_raise_err
// below) so both the "file content" side (this file) and the
// "directory/metadata" side (modos.c) report errno's consistently.
// ----------------------------------------------------------------------

int rootfs_errno_for(int fs_err);  // implemented once in modos.c

static void rootfs_raise(int fs_err) {
    mp_raise_OSError(rootfs_errno_for(fs_err));
}

// ----------------------------------------------------------------------
// file object
// ----------------------------------------------------------------------

typedef struct _mp_luos_file_obj_t {
    mp_obj_base_t base;
    fs_file_t f;
    bool open_flag;
} mp_luos_file_obj_t;

static const mp_obj_type_t mp_type_luos_file;

static void file_obj_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    mp_luos_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<file %s>", self->open_flag ? "open" : "closed");
}

static mp_uint_t file_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode) {
    mp_luos_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->open_flag) { *errcode = MP_EBADF; return MP_STREAM_ERROR; }

    int64_t n = fs_read(&self->f, buf, size);
    if (n < 0) { *errcode = rootfs_errno_for((int)n); return MP_STREAM_ERROR; }
    return (mp_uint_t)n;
}

static mp_uint_t file_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode) {
    mp_luos_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->open_flag) { *errcode = MP_EBADF; return MP_STREAM_ERROR; }
    if (!self->f.writable) { *errcode = MP_EACCES; return MP_STREAM_ERROR; }

    int64_t n = fs_write(&self->f, buf, size);
    if (n < 0) { *errcode = rootfs_errno_for((int)n); return MP_STREAM_ERROR; }
    return (mp_uint_t)n;
}

static mp_uint_t file_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    mp_luos_file_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (request == MP_STREAM_CLOSE) {
        if (self->open_flag) {
            fs_close(&self->f);
            self->open_flag = false;
        }
        return 0;
    }

    if (!self->open_flag) { *errcode = MP_EBADF; return MP_STREAM_ERROR; }

    if (request == MP_STREAM_SEEK) {
        struct mp_stream_seek_t *s = (struct mp_stream_seek_t *)arg;
        uint64_t base;
        switch (s->whence) {
            case MP_SEEK_SET: base = 0; break;
            case MP_SEEK_CUR: base = self->f.pos; break;
            case MP_SEEK_END: base = self->f.size; break;
            default: *errcode = MP_EINVAL; return MP_STREAM_ERROR;
        }
        int64_t target = (int64_t)base + s->offset;
        if (target < 0) { *errcode = MP_EINVAL; return MP_STREAM_ERROR; }

        int r = fs_seek(&self->f, (uint64_t)target);
        if (r != FS_OK) { *errcode = rootfs_errno_for(r); return MP_STREAM_ERROR; }
        s->offset = (mp_off_t)self->f.pos;
        return 0;
    }

    if (request == MP_STREAM_FLUSH) {
        return 0; // fs_write() commits synchronously in this kernel's FS layer
    }

    *errcode = MP_EINVAL;
    return MP_STREAM_ERROR;
}

static mp_obj_t file_obj_close(mp_obj_t self_in) {
    int errcode = 0;
    file_ioctl(self_in, MP_STREAM_CLOSE, 0, &errcode);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(file_obj_close_obj, file_obj_close);

static mp_obj_t file_obj___enter__(mp_obj_t self_in) {
    return self_in;
}
static MP_DEFINE_CONST_FUN_OBJ_1(file_obj___enter___obj, file_obj___enter__);

static mp_obj_t file_obj___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return file_obj_close(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(file_obj___exit___obj, 4, 4, file_obj___exit__);

static const mp_rom_map_elem_t file_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_readlines), MP_ROM_PTR(&mp_stream_unbuffered_readlines_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_seek), MP_ROM_PTR(&mp_stream_seek_obj) },
    { MP_ROM_QSTR(MP_QSTR_tell), MP_ROM_PTR(&mp_stream_tell_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&mp_stream_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&file_obj_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&file_obj___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&file_obj___exit___obj) },
};
static MP_DEFINE_CONST_DICT(file_locals_dict, file_locals_dict_table);

static const mp_stream_p_t file_stream_p = {
    .read = file_read,
    .write = file_write,
    .ioctl = file_ioctl,
    .is_text = false,
};

static MP_DEFINE_CONST_OBJ_TYPE(
    mp_type_luos_file,
    MP_QSTR_file,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    print, file_obj_print,
    protocol, &file_stream_p,
    locals_dict, &file_locals_dict
    );

// ----------------------------------------------------------------------
// open(path, mode='r') — the builtin `open()`.
//
// py/builtin.h declares `mp_builtin_open_obj` as "a port can provide
// this object" whenever MICROPY_VFS is disabled (see mpconfigport.h -
// this port has no VFS), together with a matching mp_builtin_open()
// function. That's the officially supported extension point for a
// port-specific open() with no filesystem/module-import assumptions
// baked in, so it's used here directly rather than layering another
// mechanism (like MICROPY_PORT_BUILTINS) on top - among other things,
// this also avoids MICROPY_PORT_BUILTINS' ordering trap: mpconfigport.h
// is included by py/mpconfig.h before py/obj.h even exists yet, so any
// extern mp_obj_t/MP_DECLARE_CONST_FUN_OBJ_* declaration placed
// directly in mpconfigport.h fails to compile - whereas builtin.h's
// declaration of mp_builtin_open_obj is only ever consumed later, from
// within modbuiltins.c, by which point every core type is available.
//
// Supported modes: 'r'/'rb' (read, must exist), 'w'/'wb' (write,
// create/truncate), 'a'/'ab' (append, create if missing). The 'b'
// suffix is accepted but has no effect - this port doesn't distinguish
// text/binary mode at the FS layer (no newline translation happens
// either way).
// ----------------------------------------------------------------------

mp_obj_t mp_builtin_open(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    enum { ARG_mode };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_mode, MP_ARG_OBJ, {.u_obj = MP_OBJ_NEW_QSTR(MP_QSTR_r)} },
    };
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, args + 1, kwargs,
                      MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);

    const char *path_in = mp_obj_str_get_str(args[0]);
    const char *mode = mp_obj_str_get_str(parsed[ARG_mode].u_obj);

    if (!rootfs_is_mounted()) {
        mp_raise_OSError(MP_ENODEV);
    }

    bool create = false, truncate = false, writable = false, append = false;
    switch (mode[0]) {
        case 'r': break;
        case 'w': create = true; truncate = true; writable = true; break;
        case 'a': create = true; writable = true; append = true; break;
        default:
            mp_raise_ValueError(MP_ERROR_TEXT("invalid mode"));
    }

    char path[FS_MAX_PATH];
    rootfs_normalize_path(path_in, path, sizeof(path));

    mp_luos_file_obj_t *o = mp_obj_malloc(mp_luos_file_obj_t, &mp_type_luos_file);
    memset(&o->f, 0, sizeof(o->f));
    o->open_flag = false;

    int r = fs_open(rootfs_get(), path, create, truncate, &o->f);
    if (r != FS_OK) {
        rootfs_raise(r);
    }
    o->open_flag = true;
    o->f.writable = writable;

    if (append) {
        fs_seek(&o->f, o->f.size);
    }

    return MP_OBJ_FROM_PTR(o);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, mp_builtin_open);
