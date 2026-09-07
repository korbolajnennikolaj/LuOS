#ifndef LIBC_STRING_H
#define LIBC_STRING_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *memset(void *s, int c, size_t n);

void *memcpy(void *__restrict dst, const void *__restrict src, size_t n);
void *memmove(void *dst, const void *src, size_t n);

int memcmp(const void *s1, const void *s2, size_t n);

void *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);

size_t strnlen(const char *s, size_t maxlen);

char *strcpy(char *__restrict dst, const char *__restrict src);

char *strncpy(char *__restrict dst, const char *__restrict src, size_t n);

char *strcat(char *__restrict dst, const char *__restrict src);

char *strncat(char *__restrict dst, const char *__restrict src, size_t n);

int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);

char *strchr(const char *s, int c);

char *strrchr(const char *s, int c);

char *strstr(const char *haystack, const char *needle);

size_t strspn(const char *s, const char *accept);

size_t strcspn(const char *s, const char *reject);

char *strpbrk(const char *s, const char *accept);

static inline int strcoll(const char *s1, const char *s2) {
    return strcmp(s1, s2);
}

#ifdef __cplusplus
}
#endif

#endif