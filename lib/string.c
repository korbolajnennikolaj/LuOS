#include <string.h>

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    unsigned char v = (unsigned char)c;

    while (n--)
        *p++ = v;

    return s;
}

void *memcpy(void *__restrict dst, const void *__restrict src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    while (n--)
        *d++ = *s++;

    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || n == 0)
        return dst;

    if (d < s) {

        while (n--)
            *d++ = *s++;
    } else {

        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }

    return dst;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *a = (const unsigned char *)s1;
    const unsigned char *b = (const unsigned char *)s2;

    while (n--) {
        if (*a != *b)
            return (int)*a - (int)*b;
        a++;
        b++;
    }

    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char v = (unsigned char)c;

    while (n--) {
        if (*p == v)
            return (void *)p;
        p++;
    }

    return (void *)0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t maxlen)
{
    size_t i = 0;
    while (i < maxlen && s[i])
        i++;
    return i;
}

char *strcpy(char *__restrict dst, const char *__restrict src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncpy(char *__restrict dst, const char *__restrict src, size_t n)
{
    size_t i;

    for (i = 0; i < n && src[i] != '\0'; i++)
        dst[i] = src[i];

    for (; i < n; i++)
        dst[i] = '\0';

    return dst;
}

char *strcat(char *__restrict dst, const char *__restrict src)
{
    char *d = dst;
    while (*d)
        d++;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncat(char *__restrict dst, const char *__restrict src, size_t n)
{
    char *d = dst;
    while (*d)
        d++;
    while (n-- && *src)
        *d++ = *src++;
    *d = '\0';
    return dst;
}

int strcmp(const char *s1, const char *s2)
{
    const unsigned char *a = (const unsigned char *)s1;
    const unsigned char *b = (const unsigned char *)s2;

    while (*a && *a == *b) {
        a++;
        b++;
    }

    return (int)*a - (int)*b;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    const unsigned char *a = (const unsigned char *)s1;
    const unsigned char *b = (const unsigned char *)s2;

    while (n--) {
        if (*a != *b)
            return (int)*a - (int)*b;
        if (*a == '\0')
            return 0;
        a++;
        b++;
    }

    return 0;
}

char *strchr(const char *s, int c)
{
    unsigned char ch = (unsigned char)c;

    for (;;) {
        if ((unsigned char)*s == ch)
            return (char *)s;
        if (*s == '\0')
            return (char *)0;
        s++;
    }
}

char *strrchr(const char *s, int c)
{
    unsigned char ch = (unsigned char)c;
    const char *last = (char *)0;

    for (;;) {
        if ((unsigned char)*s == ch)
            last = s;
        if (*s == '\0')
            break;
        s++;
    }

    return (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    if (!*needle)
        return (char *)haystack;

    for (; *haystack; haystack++) {
        if (*haystack != *needle)
            continue;

        const char *h = haystack;
        const char *n = needle;

        while (*n && *h == *n) {
            h++;
            n++;
        }

        if (!*n)
            return (char *)haystack;
    }

    return (char *)0;
}

size_t strspn(const char *s, const char *accept)
{
    const char *p;
    const char *a;
    size_t count = 0;

    for (p = s; *p; p++) {
        for (a = accept; *a; a++) {
            if (*p == *a)
                goto next;
        }
        break;
    next:
        count++;
    }

    return count;
}

size_t strcspn(const char *s, const char *reject)
{
    const char *p;
    const char *r;
    size_t count = 0;

    for (p = s; *p; p++) {
        for (r = reject; *r; r++) {
            if (*p == *r)
                return count;
        }
        count++;
    }

    return count;
}

char *strpbrk(const char *s, const char *accept)
{
    for (; *s; s++) {
        const char *a;
        for (a = accept; *a; a++) {
            if (*s == *a)
                return (char *)s;
        }
    }

    return (char *)0;
}