#include "luos_file.h"

#include "kernel/console.h"
#include "kernel/rootfs.h"
#include <stdarg.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

FILE __luos_stdin = { .fd = 0, .eof = 0, .err = 0, .ungetc_char = -1,
                      .mem_buf = 0, .mem_pos = 0, .mem_len = 0,
                      .is_disk = 0 };
FILE __luos_stdout = { .fd = 1, .eof = 0, .err = 0, .ungetc_char = -1,
                       .mem_buf = 0, .mem_pos = 0, .mem_len = 0,
                       .is_disk = 0 };
FILE __luos_stderr = { .fd = 2, .eof = 0, .err = 0, .ungetc_char = -1,
                       .mem_buf = 0, .mem_pos = 0, .mem_len = 0,
                       .is_disk = 0 };

static int fs_err_to_errno(int fs_err) {
    switch (fs_err) {
        case FS_OK: return 0;
        case FS_ERR_NOENT: return ENOENT;
        case FS_ERR_EXIST: return EEXIST;
        case FS_ERR_NOTDIR: return ENOTDIR;
        case FS_ERR_ISDIR: return EISDIR;
        case FS_ERR_NOSPACE: return ENOSPC;
        case FS_ERR_PARAM: return EINVAL;
        case FS_ERR_NOSUPP: return EPERM;
        case FS_ERR_NOMEM: return ENOMEM;
        case FS_ERR_CORRUPT: return EIO;
        case FS_ERR_NOTMOUNTED: return ENODEV;
        case FS_ERR_NOTEMPTY: return EEXIST;
        case FS_ERR_EOF: return 0;
        case FS_ERR_IO: return EIO;
        default: return EIO;
    }
}

FILE *fopen(const char *path, const char *mode) {
    if (!path || !mode || !mode[0]) { errno = EINVAL; return (FILE*)0; }

    if (!rootfs_is_mounted()) {
        errno = ENODEV;
        return (FILE*)0;
    }

    bool create = false, truncate = false, writable = false, append = false;
    switch (mode[0]) {
        case 'r': break;
        case 'w': create = true; truncate = true; writable = true; break;
        case 'a': create = true; writable = true; append = true; break;
        default: errno = EINVAL; return (FILE*)0;
    }
    if (mode[1] == '+' || (mode[1] == 'b' && mode[2] == '+')) {
        errno = EINVAL;
        return (FILE*)0;
    }

    char norm_path[FS_MAX_PATH];
    rootfs_normalize_path(path, norm_path, sizeof(norm_path));

    FILE *f = (FILE*)malloc(sizeof(FILE));
    if (!f) { errno = ENOMEM; return (FILE*)0; }
    memset(f, 0, sizeof(FILE));
    f->fd = -1;
    f->ungetc_char = -1;

    int r = fs_open(rootfs_get(), norm_path, create, truncate, &f->disk);
    if (r != FS_OK) {
        free(f);
        errno = fs_err_to_errno(r);
        return (FILE*)0;
    }
    f->disk.writable = writable;
    f->is_disk = 1;

    if (append) {
        fs_seek(&f->disk, f->disk.size);
    }

    return f;
}

FILE *freopen(const char *path, const char *mode, FILE *stream) {
    if (!stream) return fopen(path, mode);
    fclose(stream);
    FILE *nf = fopen(path, mode);
    if (!nf) return (FILE*)0;
    *stream = *nf;
    free(nf);
    return stream;
}

int fclose(FILE *stream) {
    if (!stream) return EOF;

    if (stream->is_disk) {
        fs_close(&stream->disk);
        stream->is_disk = 0;
    }

    if (stream->fd >= 3) {
        stream->fd = -1;
        free(stream);
    }
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!stream || !ptr || size == 0 || nmemb == 0) return 0;

    if (stream->is_disk) {
        int64_t n = fs_read(&stream->disk, ptr, (uint64_t)(size * nmemb));
        if (n < 0) { stream->err = 1; return 0; }
        if ((uint64_t)n < size * nmemb) stream->eof = 1;
        return (size_t)n / size;
    }

    if (stream->fd == 0 && !stream->mem_buf) {
        unsigned char *dst = (unsigned char *)ptr;
        size_t want = size * nmemb;
        size_t got = 0;

        while (got < want) {
            int c = fgetc(stream);
            if (c == EOF) break;
            dst[got++] = (unsigned char)c;
        }

        if (got < want) stream->eof = 1;
        return got / size;
    }

    if (stream->mem_buf && stream->mem_pos < stream->mem_len) {
        size_t total = size * nmemb;
        size_t avail = stream->mem_len - stream->mem_pos;
        if (total > avail) total = avail;
        memcpy(ptr, stream->mem_buf + stream->mem_pos, total);
        stream->mem_pos += total;
        if (stream->mem_pos >= stream->mem_len) stream->eof = 1;
        return total / size;
    }

    stream->eof = 1;
    return 0;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!stream || !ptr || size == 0 || nmemb == 0) return 0;

    size_t total = size * nmemb;

    if (stream->is_disk) {
        if (!stream->disk.writable) { stream->err = 1; return 0; }
        int64_t n = fs_write(&stream->disk, ptr, (uint64_t)total);
        if (n < 0) { stream->err = 1; return 0; }
        return (size_t)n / size;
    }

    const char *s = (const char *)ptr;

    if (stream->fd == 1 || stream->fd == 2) {

        char buf[256];
        size_t written = 0;
        while (written < total) {
            size_t chunk = total - written;
            if (chunk > 255) chunk = 255;
            memcpy(buf, s + written, chunk);
            buf[chunk] = '\0';

            if (stream->fd == 1)
                printf("%s", buf);
            else

                printf("%s", buf);

            written += chunk;
        }
        return nmemb;
    }

    stream->err = 1;
    return 0;
}

int fgetc(FILE *stream) {
    if (!stream) return EOF;

    if (stream->ungetc_char >= 0) {
        int c = stream->ungetc_char;
        stream->ungetc_char = -1;
        return c;
    }

    if (stream->is_disk) {
        unsigned char c;
        int64_t n = fs_read(&stream->disk, &c, 1);
        if (n != 1) { stream->eof = 1; return EOF; }
        return (int)c;
    }

    if (stream->mem_buf && stream->mem_pos < stream->mem_len) {
        return (unsigned char)stream->mem_buf[stream->mem_pos++];
    }

    if (stream->fd == 0 && !stream->mem_buf) {
        int c = luos_console_getc();
        if (c == LUOS_CONSOLE_EOF) { stream->eof = 1; return EOF; }
        stream->eof = 0;
        return c;
    }

    stream->eof = 1;
    return EOF;
}

int fputc(int c, FILE *stream) {
    char ch = (char)c;
    if (fwrite(&ch, 1, 1, stream) == 1) return (unsigned char)c;
    return EOF;
}

int fputs(const char *s, FILE *stream) {
    if (!s || !stream) return EOF;
    size_t len = strlen(s);
    if (fwrite(s, 1, len, stream) == len) return 0;
    return EOF;
}

char *fgets(char *s, int size, FILE *stream) {
    if (!s || size <= 0 || !stream) return (char*)0;
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(stream);
        if (c == EOF) {
            if (i == 0) return (char*)0;
            break;
        }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

int ungetc(int c, FILE *stream) {
    if (!stream || c == EOF) return EOF;
    stream->ungetc_char = c;
    stream->eof = 0;
    return c;
}

int fprintf(FILE *stream, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (stream) {
        fwrite(buf, 1, (n > 0 && (size_t)n < sizeof(buf)) ? (size_t)n : strlen(buf), stream);
    }
    return n;
}

int vfprintf(FILE *stream, const char *fmt, ...) {

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (stream) fwrite(buf, 1, (size_t)n, stream);
    return n;
}

int fseek(FILE *stream, long offset, int whence) {
    if (!stream) return -1;

    if (stream->is_disk) {
        uint64_t base;
        switch (whence) {
            case SEEK_SET: base = 0; break;
            case SEEK_CUR: base = stream->disk.pos; break;
            case SEEK_END: base = stream->disk.size; break;
            default: return -1;
        }
        long target = (long)base + offset;
        if (target < 0) return -1;
        if (fs_seek(&stream->disk, (uint64_t)target) != FS_OK) return -1;
        stream->eof = 0;
        return 0;
    }

    if (stream->mem_buf) {
        size_t newpos;
        switch (whence) {
            case SEEK_SET: newpos = (size_t)offset; break;
            case SEEK_CUR: newpos = stream->mem_pos + (size_t)offset; break;
            case SEEK_END: newpos = stream->mem_len + (size_t)offset; break;
            default: return -1;
        }
        if (newpos > stream->mem_len) return -1;
        stream->mem_pos = newpos;
        stream->eof = 0;
        return 0;
    }
    return -1;
}

long ftell(FILE *stream) {
    if (!stream) return -1;
    if (stream->is_disk) return (long)stream->disk.pos;
    if (stream->mem_buf) return (long)stream->mem_pos;
    return -1;
}

void rewind(FILE *stream) {
    if (stream) {
        fseek(stream, 0, SEEK_SET);
        stream->err = 0;
    }
}

int feof(FILE *stream) { return stream ? stream->eof : 1; }
int ferror(FILE *stream) { return stream ? stream->err : 1; }
void clearerr(FILE *stream) {
    if (!stream) return;
    stream->eof = 0;
    stream->err = 0;

    if (stream->fd == 0 && !stream->is_disk && !stream->mem_buf) {
        luos_console_flush_input();
    }
}
int fflush(FILE *stream) { (void)stream; return 0; }

FILE *tmpfile(void) { return (FILE*)0; }
char *tmpnam(char *s) { (void)s; return (char*)0; }

int remove(const char *pathname) {
    if (!rootfs_is_mounted()) { errno = ENODEV; return -1; }

    char norm_path[FS_MAX_PATH];
    rootfs_normalize_path(pathname, norm_path, sizeof(norm_path));

    fs_t *fs = rootfs_get();
    fs_dirent_t st;
    int r = fs_stat(fs, norm_path, &st);
    if (r != FS_OK) { errno = fs_err_to_errno(r); return -1; }

    r = (st.type == FS_ENTRY_DIR) ? fs_rmdir(fs, norm_path) : fs_unlink(fs, norm_path);
    if (r != FS_OK) { errno = fs_err_to_errno(r); return -1; }
    return 0;
}

int rename(const char *oldpath, const char *newpath) {

    (void)oldpath; (void)newpath;
    errno = ENOSYS;
    return -1;
}

int setvbuf(FILE *stream, char *buf, int mode, size_t size) {
    (void)stream; (void)buf; (void)mode; (void)size;
    return 0;
}
