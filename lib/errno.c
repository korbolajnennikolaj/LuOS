#include <errno.h>
#include <stddef.h>

int errno = 0;

static const char * const _err_table[] = {
    "Success",
    "Operation not permitted",
    "No such file or directory",
    "No such process",
    "Interrupted system call",
    "Input/output error",
    "No such device or address",
    "Argument list too long",
    "Exec format error",
    "Bad file descriptor",
    "No child processes",
    "Resource temporarily unavailable",
    "Cannot allocate memory",
    "Permission denied",
    "Bad address",
    (const char*)0,
    "Device or resource busy",
    "File exists",
    "Invalid cross-device link",
    "No such device",
    "Not a directory",
    "Is a directory",
    "Invalid argument",
    "Too many open files in system",
    "Too many open files",
    "Inappropriate ioctl for device",
    (const char*)0,
    "File too large",
    "No space left on device",
    "Illegal seek",
    "Read-only file system",
    "Too many links",
    "Broken pipe",
    "Numerical argument out of domain",
    "Numerical result out of range",
    "Resource deadlock avoided",
    "File name too long",
    "No locks available",
    "Function not implemented",
    "Directory not empty",
    "Too many levels of symbolic links",
    (const char*)0,
    "No message of desired type",
    "Identifier removed",
};

#define ERR_TABLE_SIZE ((int)(sizeof(_err_table)/sizeof(_err_table[0])))

static const struct { int code; const char *msg; } _err_ext[] = {
    { 60, "Device not a stream" },
    { 61, "No data available" },
    { 62, "Timer expired" },
    { 63, "Out of streams resources" },
    { 66, "Object is remote" },
    { 67, "Link has been severed" },
    { 71, "Protocol error" },
    { 72, "Multihop attempted" },
    { 74, "Bad message" },
    { 75, "Value too large for defined data type" },
    { 84, "Invalid or incomplete multibyte character" },
    { 87, "Too many users" },
    { 88, "Socket operation on non-socket" },
    { 95, "Operation not supported" },
    { 97, "Address family not supported by protocol" },
    { 98, "Address already in use" },
    { 104, "Connection reset by peer" },
    { 105, "No buffer space available" },
    { 106, "Transport endpoint is already connected" },
    { 107, "Transport endpoint is not connected" },
    { 110, "Connection timed out" },
    { 111, "Connection refused" },
    { 113, "No route to host" },
    { 114, "Operation already in progress" },
    { 115, "Operation now in progress" },
    { 116, "Stale file handle" },
    { 0, (const char*)0 }
};

const char *strerror(int errnum)
{

    if (errnum >= 0 && errnum < ERR_TABLE_SIZE) {
        const char *s = _err_table[errnum];
        if (s) return s;
    }

    for (int i = 0; _err_ext[i].msg != (const char*)0; i++) {
        if (_err_ext[i].code == errnum)
            return _err_ext[i].msg;
    }

    return "Unknown error";
}

int strerror_r(int errnum, char *buf, size_t buflen)
{
    if (!buf || buflen == 0) return EINVAL;

    const char *msg = strerror(errnum);
    size_t i = 0;
    while (i < buflen - 1 && msg[i]) {
        buf[i] = msg[i];
        i++;
    }
    buf[i] = '\0';
    return 0;
}