#ifndef LUOS_FILE_H
#define LUOS_FILE_H

#include "fs/fs.h"

#include <stddef.h>
#include <stdint.h>

#define _LUOS_FILE_BUF_SIZE 512

typedef struct _LUOS_FILE {
    int fd;
    int eof;
    int err;
    int ungetc_char;

    const char *mem_buf;
    size_t mem_pos;
    size_t mem_len;

    int is_disk;
    fs_file_t disk;
} FILE;

extern FILE __luos_stdin;
extern FILE __luos_stdout;
extern FILE __luos_stderr;

#define stdin (&__luos_stdin)
#define stdout (&__luos_stdout)
#define stderr (&__luos_stderr)

#define EOF (-1)
#define BUFSIZ 512
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define FILENAME_MAX 256
#define FOPEN_MAX 8
#define TMP_MAX 25
#define L_tmpnam 20

#ifndef _IOFBF
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#endif

FILE *fopen(const char *path, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *stream);
int fclose(FILE *stream);

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);

int fgetc(FILE *stream);
int fputc(int c, FILE *stream);
int fputs(const char *s, FILE *stream);
char *fgets(char *s, int size, FILE *stream);
int ungetc(int c, FILE *stream);

int fprintf(FILE *stream, const char *fmt, ...);
int vfprintf(FILE *stream, const char *fmt, ...);

int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
void rewind(FILE *stream);

int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);
int fflush(FILE *stream);

FILE *tmpfile(void);
char *tmpnam(char *s);
int remove(const char *pathname);
int rename(const char *oldpath, const char *newpath);

int setvbuf(FILE *stream, char *buf, int mode, size_t size);
#define setbuf(stream, buf) setvbuf(stream, buf, (buf) ? _IOFBF : _IONBF, BUFSIZ)

#define getc(f) fgetc(f)
#define putc(c,f) fputc((c),(f))

static inline FILE *popen(const char *cmd, const char *mode) {
    (void)cmd; (void)mode; return (FILE*)0;
}
static inline int pclose(FILE *stream) {
    (void)stream; return -1;
}

#endif
