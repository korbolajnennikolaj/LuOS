#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7FFFFFFF

void *malloc (size_t size);
void *calloc (size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free (void *ptr);
void *aligned_alloc(size_t alignment, size_t size);

double strtod (const char *nptr, char **endptr);
float strtof (const char *nptr, char **endptr);
long strtol (const char *nptr, char **endptr, int base);
unsigned long strtoul (const char *nptr, char **endptr, int base);
long long strtoll (const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);

static inline int atoi(const char *s) { return (int)strtol(s, (char**)0, 10); }
static inline long atol(const char *s) { return strtol(s, (char**)0, 10); }
static inline double atof(const char *s) { return strtod(s, (char**)0); }

static inline int abs (int x) { return x < 0 ? -x : x; }
static inline long labs (long x) { return x < 0 ? -x : x; }
static inline long long llabs(long long x) { return x < 0 ? -x : x; }

typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;

static inline div_t div (int a, int b) { div_t r={a/b,a%b}; return r; }
static inline ldiv_t ldiv (long a, long b) { ldiv_t r={a/b,a%b}; return r; }
static inline lldiv_t lldiv(long long a, long long b) { lldiv_t r={a/b,a%b}; return r; }

int rand (void);
void srand(unsigned int seed);

void qsort (void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));

__attribute__((noreturn)) void abort(void);
__attribute__((noreturn)) void exit (int status);
__attribute__((noreturn)) void _Exit(int status);
int atexit(void (*func)(void));

static inline char *getenv(const char *name) { (void)name; return (char*)0; }

static inline int system(const char *cmd) { (void)cmd; return -1; }

#ifdef __cplusplus
}
#endif

#endif