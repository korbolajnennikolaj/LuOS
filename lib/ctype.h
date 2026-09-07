#ifndef CTYPE_H
#define CTYPE_H

static inline int isdigit (int c) { return (unsigned)(c - '0') < 10u; }
static inline int isupper (int c) { return (unsigned)(c - 'A') < 26u; }
static inline int islower (int c) { return (unsigned)(c - 'a') < 26u; }
static inline int isalpha (int c) { return (unsigned)((c | 32) - 'a') < 26u; }
static inline int isalnum (int c) { return isalpha(c) || isdigit(c); }

static inline int isspace(int c) {
    return c == ' ' || (unsigned)(c - '\t') < 5u;
}

static inline int isxdigit(int c) {
    return isdigit(c) || (unsigned)((c | 32) - 'a') < 6u;
}

static inline int isprint (int c) { return (unsigned)(c - ' ') < 95u; }
static inline int isgraph (int c) { return (unsigned)(c - '!') < 94u; }
static inline int iscntrl (int c) { return (unsigned)c < 32u || c == 127; }
static inline int ispunct (int c) { return isgraph(c) && !isalnum(c); }
static inline int isblank (int c) { return c == ' ' || c == '\t'; }
static inline int isascii (int c) { return (unsigned)c < 128u; }

static inline int tolower(int c) { return isupper(c) ? (c | 32) : c; }
static inline int toupper(int c) { return islower(c) ? (c & ~32) : c; }
static inline int toascii(int c) { return c & 0x7F; }

#endif