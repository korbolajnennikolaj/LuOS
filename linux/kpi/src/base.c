// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include <lkpi/all.h>
#include "luos_glue.h"


struct pbuf {
	char *buf;
	size_t size;
	size_t len;
};

static void pb_putc(struct pbuf *b, char c)
{
	if (b->len + 1 < b->size) b->buf[b->len] = c;
	b->len++;
}

static void pb_puts(struct pbuf *b, const char *s, int width, int prec, bool left)
{
	size_t n = 0;
	if (!s) s = "(null)";
	while (s[n] && (prec < 0 || (int)n < prec)) n++;
	int pad = width > (int)n ? width - (int)n : 0;
	if (!left) while (pad-- > 0) pb_putc(b, ' ');
	for (size_t i = 0; i < n; i++) pb_putc(b, s[i]);
	if (left) while (pad-- > 0) pb_putc(b, ' ');
}

static void pb_num(struct pbuf *b, unsigned long long v, bool neg, int base, bool upper, int width, int prec,
		   bool left, bool zero, bool plus, bool space, bool alt)
{
	char tmp[66];
	int n = 0;
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	if (v == 0 && prec != 0) tmp[n++] = '0';
	while (v) {
		tmp[n++] = digits[v % base];
		v /= base;
	}
	while (n < prec) tmp[n++] = '0';
	char sign = neg ? '-' : plus ? '+' : space ? ' ' : 0;
	int prefix = (alt && base == 16) ? 2 : (alt && base == 8 && tmp[n - 1] != '0') ? 1 : 0;
	int total = n + (sign ? 1 : 0) + prefix;
	int pad = width > total ? width - total : 0;
	if (!left && !zero) while (pad-- > 0) pb_putc(b, ' ');
	if (sign) pb_putc(b, sign);
	if (prefix) {
		pb_putc(b, '0');
		if (base == 16) pb_putc(b, upper ? 'X' : 'x');
	}
	if (!left && zero) while (pad-- > 0) pb_putc(b, '0');
	while (n) pb_putc(b, tmp[--n]);
	if (left) while (pad-- > 0) pb_putc(b, ' ');
}

static const char *const errnames[] = {
	[EPERM] = "EPERM", [ENOENT] = "ENOENT", [ESRCH] = "ESRCH", [EINTR] = "EINTR", [EIO] = "EIO",
	[ENXIO] = "ENXIO", [E2BIG] = "E2BIG", [ENOEXEC] = "ENOEXEC", [EBADF] = "EBADF", [ECHILD] = "ECHILD",
	[EAGAIN] = "EAGAIN", [ENOMEM] = "ENOMEM", [EACCES] = "EACCES", [EFAULT] = "EFAULT", [EBUSY] = "EBUSY",
	[EEXIST] = "EEXIST", [ENODEV] = "ENODEV", [ENOTDIR] = "ENOTDIR", [EISDIR] = "EISDIR", [EINVAL] = "EINVAL",
	[ENFILE] = "ENFILE", [EMFILE] = "EMFILE", [ENOTTY] = "ENOTTY", [EFBIG] = "EFBIG", [ENOSPC] = "ENOSPC",
	[ESPIPE] = "ESPIPE", [EROFS] = "EROFS", [EPIPE] = "EPIPE", [ERANGE] = "ERANGE", [EDEADLK] = "EDEADLK",
	[ENOSYS] = "ENOSYS", [ETIMEDOUT] = "ETIMEDOUT", [EOPNOTSUPP] = "EOPNOTSUPP", [ENOTSUPP] = "ENOTSUPP",
	[EPROBE_DEFER] = "EPROBE_DEFER", [ECANCELED] = "ECANCELED", [ENODATA] = "ENODATA", [ENOENT] = "ENOENT",
	[ERESTARTSYS] = "ERESTARTSYS", [EALREADY] = "EALREADY", [ENOLINK] = "ENOLINK", [EOVERFLOW] = "EOVERFLOW",
};

static const char *pointer_ext(struct pbuf *b, const char *fmt, void *ptr, int width, int prec, bool left)
{
	char tmp[128];
	switch (*fmt) {
	case 'V': {
		struct va_format *vaf = ptr;
		va_list va;
		va_copy(va, *vaf->va);
		char *dst = b->len < b->size ? b->buf + b->len : NULL;
		size_t room = b->len < b->size ? b->size - b->len : 0;
		int n = lkpi_vsnprintf(dst, room, vaf->fmt, va);
		va_end(va);
		b->len += n;
		return fmt + 1;
	}
	case '4':
		if (fmt[1] == 'c' && (fmt[2] == 'c' || fmt[2] == 'h' || fmt[2] == 'l' || fmt[2] == 'b')) {
			u32 v = ptr ? *(const u32 *)ptr : 0;
			int p = 0;
			for (int i = 0; i < 4; i++) {
				unsigned char c = (v >> (i * 8)) & 0x7F;
				tmp[p++] = (c >= 32 && c < 127) ? c : '.';
			}
			p += lkpi_snprintf(tmp + p, sizeof(tmp) - p, " %s-endian (0x%08x)", (v & (1U << 31)) ? "big" : "little", v);
			pb_puts(b, tmp, width, -1, left);
			return fmt + 3;
		}
		break;
	case 'e': {
		long err = (long)ptr;
		if (err < 0 && -err < (long)ARRAY_SIZE(errnames) && errnames[-err]) {
			pb_putc(b, '-');
			pb_puts(b, errnames[-err], 0, -1, false);
		} else {
			pb_num(b, err < 0 ? -err : err, err < 0, 10, false, 0, -1, false, false, false, false, false);
		}
		return fmt + 1;
	}
	case 'h': {
		const u8 *p = ptr;
		char sep = ' ';
		fmt++;
		if (*fmt == 'C') { sep = ':'; fmt++; }
		else if (*fmt == 'D') { sep = '-'; fmt++; }
		else if (*fmt == 'N') { sep = 0; fmt++; }
		int n = width > 0 ? width : 1;
		for (int i = 0; i < n && p; i++) {
			if (i && sep) pb_putc(b, sep);
			pb_putc(b, "0123456789abcdef"[p[i] >> 4]);
			pb_putc(b, "0123456789abcdef"[p[i] & 15]);
		}
		return fmt;
	}
	case 'a': {
		unsigned long long v = ptr ? *(const u64 *)ptr : 0;
		fmt++;
		if (*fmt == 'd' || *fmt == 'p') fmt++;
		pb_num(b, v, false, 16, false, 18, 16, false, true, false, false, true);
		return fmt;
	}
	case 'R':
	case 'r': {
		const struct resource *r = ptr;
		if (r) lkpi_snprintf(tmp, sizeof(tmp), "[%s %#010llx-%#010llx]", (r->flags & IORESOURCE_IO) ? "io" : "mem",
				     (unsigned long long)r->start, (unsigned long long)r->end);
		else strcpy(tmp, "[??]");
		pb_puts(b, tmp, width, -1, left);
		return fmt + 1;
	}
	case 'D':
	case 'd': {
		const struct file *f = ptr;
		pb_puts(b, f ? "file" : "(null)", width, prec, left);
		fmt++;
		while (*fmt >= '0' && *fmt <= '4') fmt++;
		return fmt;
	}
	case 'b': {
		const unsigned long *bm = ptr;
		bool list = fmt[1] == 'l';
		int nbits = width > 0 ? width : 0;
		if (list) {
			bool first = true;
			for (int i = 0; i < nbits; i++) {
				if (!test_bit(i, bm)) continue;
				int j = i;
				while (j + 1 < nbits && test_bit(j + 1, bm)) j++;
				lkpi_snprintf(tmp, sizeof(tmp), i == j ? "%s%d" : "%s%d-%d", first ? "" : ",", i, j);
				pb_puts(b, tmp, 0, -1, false);
				first = false;
				i = j;
			}
			return fmt + 2;
		}
		for (int i = ((nbits + 31) / 32) - 1; i >= 0; i--) {
			u32 w = (u32)(bm[i / 2] >> ((i % 2) * 32));
			lkpi_snprintf(tmp, sizeof(tmp), "%08x%s", w, i ? "," : "");
			pb_puts(b, tmp, 0, -1, false);
		}
		return fmt + 1;
	}
	case 'O':
		pb_puts(b, "<of-node>", width, prec, left);
		fmt++;
		while (isalpha(*fmt)) fmt++;
		return fmt;
	case 'S': case 's': case 'B': case 'F': case 'f':
		pb_num(b, (unsigned long)ptr, false, 16, false, 0, -1, false, false, false, false, true);
		fmt++;
		if (*fmt == 'R' || *fmt == 'b') fmt++;
		return fmt;
	case 'K': case 'x': case 'X':
		fmt++;
		break;
	case 'U': case 'M': case 'm': case 'I': case 'i': case 'C': case 'G': case 'g': case 't': case 'N': case 'E':
		fmt++;
		while (isalnum(*fmt)) fmt++;
		pb_num(b, (unsigned long)ptr, false, 16, false, 0, -1, false, false, false, false, true);
		return fmt;
	default:
		break;
	}
	if (!ptr) {
		pb_puts(b, "(null)", width, -1, left);
		return fmt;
	}
	pb_num(b, (unsigned long)ptr, false, 16, false, width, -1, left, false, false, false, true);
	return fmt;
}

int lkpi_vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
	struct pbuf b = { buf, size, 0 };
	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			pb_putc(&b, *fmt);
			continue;
		}
		const char *start = fmt;
		bool left = false, zero = false, plus = false, space = false, alt = false;
		for (;;) {
			char c = *++fmt;
			if (c == '-') left = true;
			else if (c == '0') zero = true;
			else if (c == '+') plus = true;
			else if (c == ' ') space = true;
			else if (c == '#') alt = true;
			else break;
		}
		int width = -1, prec = -1;
		if (*fmt == '*') {
			width = va_arg(args, int);
			if (width < 0) { left = true; width = -width; }
			fmt++;
		} else {
			while (*fmt >= '0' && *fmt <= '9') width = (width < 0 ? 0 : width) * 10 + (*fmt++ - '0');
		}
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			if (*fmt == '*') { prec = va_arg(args, int); fmt++; }
			else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
			if (prec >= 0) zero = false;
		}
		int lng = 0;
		bool sz = false;
		for (;;) {
			if (*fmt == 'l') { lng++; fmt++; }
			else if (*fmt == 'h') { lng--; fmt++; }
			else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j' || *fmt == 'L' || *fmt == 'q') { sz = true; fmt++; }
			else break;
		}
		if (width < 0) width = 0;
		switch (*fmt) {
		case 'd':
		case 'i': {
			long long v;
			if (sz || lng >= 1) v = lng >= 2 ? va_arg(args, long long) : va_arg(args, long);
			else v = va_arg(args, int);
			if (lng == -1) v = (short)v;
			if (lng <= -2) v = (signed char)v;
			pb_num(&b, v < 0 ? -(unsigned long long)v : (unsigned long long)v, v < 0, 10, false, width, prec, left,
			       zero, plus, space, false);
			break;
		}
		case 'u': case 'x': case 'X': case 'o': case 'b': {
			unsigned long long v;
			if (sz || lng >= 1) v = lng >= 2 ? va_arg(args, unsigned long long) : va_arg(args, unsigned long);
			else v = va_arg(args, unsigned int);
			if (lng == -1) v = (unsigned short)v;
			if (lng <= -2) v = (unsigned char)v;
			int base = *fmt == 'o' ? 8 : *fmt == 'b' ? 2 : *fmt == 'u' ? 10 : 16;
			pb_num(&b, v, false, base, *fmt == 'X', width, prec, left, zero, false, false, alt);
			break;
		}
		case 'c':
			if (!left) while (width-- > 1) pb_putc(&b, ' ');
			pb_putc(&b, (char)va_arg(args, int));
			if (left) while (width-- > 1) pb_putc(&b, ' ');
			break;
		case 's':
			pb_puts(&b, va_arg(args, const char *), width, prec, left);
			break;
		case 'p':
			fmt = pointer_ext(&b, fmt + 1, va_arg(args, void *), width, prec, left) - 1;
			break;
		case '%':
			pb_putc(&b, '%');
			break;
		case 'n':
			break;
		default:
			while (start <= fmt && *start) pb_putc(&b, *start++);
			if (!*fmt) fmt--;
			break;
		}
	}
	if (size) b.buf[b.len < size ? b.len : size - 1] = 0;
	return (int)b.len;
}

int lkpi_snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	int r = lkpi_vsnprintf(buf, size, fmt, va);
	va_end(va);
	return r;
}

int lkpi_vsprintf(char *buf, const char *fmt, va_list args) { return lkpi_vsnprintf(buf, INT_MAX, fmt, args); }

int lkpi_sprintf(char *buf, const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	int r = lkpi_vsnprintf(buf, INT_MAX, fmt, va);
	va_end(va);
	return r;
}

int vscnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
	int i = lkpi_vsnprintf(buf, size, fmt, args);
	if (likely(i < (int)size)) return i;
	return size ? (int)size - 1 : 0;
}

int scnprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	int r = vscnprintf(buf, size, fmt, va);
	va_end(va);
	return r;
}

int sysfs_emit(char *buf, const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	int r = vscnprintf(buf, PAGE_SIZE, fmt, va);
	va_end(va);
	return r;
}

int sysfs_emit_at(char *buf, int at, const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	int r = vscnprintf(buf + at, PAGE_SIZE - at, fmt, va);
	va_end(va);
	return r;
}

char *kvasprintf(gfp_t gfp, const char *fmt, va_list ap)
{
	va_list aq;
	va_copy(aq, ap);
	int len = lkpi_vsnprintf(NULL, 0, fmt, aq);
	va_end(aq);
	char *p = kmalloc(len + 1, gfp);
	if (!p) return NULL;
	lkpi_vsnprintf(p, len + 1, fmt, ap);
	return p;
}

const char *kvasprintf_const(gfp_t gfp, const char *fmt, va_list ap) { return kvasprintf(gfp, fmt, ap); }

char *kasprintf(gfp_t gfp, const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	char *p = kvasprintf(gfp, fmt, va);
	va_end(va);
	return p;
}


static int printk_level(const char **fmtp)
{
	const char *f = *fmtp;
	int level = LOGLEVEL_INFO;
	while (f[0] == KERN_SOH_ASCII && f[1]) {
		if (f[1] >= '0' && f[1] <= '7') level = f[1] - '0';
		else if (f[1] == 'c') level = -1;
		f += 2;
	}
	*fmtp = f;
	return level;
}

static int luos_level(int level)
{
	if (level < 0) return 1;
	if (level <= LOGLEVEL_ERR) return 3;
	if (level == LOGLEVEL_WARNING) return 2;
	if (level == LOGLEVEL_DEBUG) return 0;
	return 1;
}

static void emit_lines(int level, char *msg)
{
	char *line = msg;
	for (char *p = msg;; p++) {
		if (*p == '\n' || *p == 0) {
			char c = *p;
			*p = 0;
			if (*line) luos_log(luos_level(level), line);
			if (!c) break;
			line = p + 1;
		}
	}
}

int vprintk(const char *fmt, va_list args)
{
	char buf[1024];
	int n = lkpi_vsnprintf(buf, sizeof(buf), fmt, args);
	const char *msg = buf;
	int level = printk_level(&msg);
	emit_lines(level, buf + (msg - buf));
	return n;
}

int _printk(const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	int r = vprintk(fmt, va);
	va_end(va);
	return r;
}

struct ratelimit_state printk_ratelimit_state =
	RATELIMIT_STATE_INIT(printk_ratelimit_state, DEFAULT_RATELIMIT_INTERVAL, DEFAULT_RATELIMIT_BURST);

int ___ratelimit(struct ratelimit_state *rs, const char *func)
{
	int ret, missed = 0;
	if (rs->interval <= 0) return 1;
	if (__atomic_exchange_n(&rs->lock, 1, __ATOMIC_ACQUIRE)) return 0;
	unsigned long now = jiffies;
	if (!rs->begin || time_after_eq(now, rs->begin + rs->interval)) {
		missed = rs->missed;
		rs->begin = now;
		rs->printed = 0;
		rs->missed = 0;
	}
	if (rs->burst > rs->printed) {
		rs->printed++;
		ret = 1;
	} else {
		rs->missed++;
		ret = 0;
	}
	__atomic_store_n(&rs->lock, 0, __ATOMIC_RELEASE);
	if (missed) _printk(KERN_WARNING "%s: %d callbacks suppressed\n", func, missed);
	return ret;
}

void dump_stack(void)
{
	void *ra = __builtin_return_address(0);
	_printk(KERN_WARNING "stack: called from %px\n", ra);
}

void lkpi_bug(const char *file, int line, const char *func)
{
	char msg[256];
	lkpi_snprintf(msg, sizeof(msg), "BUG at %s:%d in %s()", file, line, func);
	luos_panic(msg);
}

void lkpi_warn(const char *file, int line, const char *func, const char *fmt, ...)
{
	char msg[512];
	int n = lkpi_snprintf(msg, sizeof(msg), "WARNING at %s:%d %s(): ", file, line, func);
	if (fmt) {
		va_list va;
		va_start(va, fmt);
		lkpi_vsnprintf(msg + n, sizeof(msg) - n, fmt, va);
		va_end(va);
	}
	luos_log(2, msg);
}

void lkpi_panic(const char *fmt, ...)
{
	char msg[512];
	va_list va;
	va_start(va, fmt);
	lkpi_vsnprintf(msg, sizeof(msg), fmt, va);
	va_end(va);
	luos_panic(msg);
}


char *strncat(char *d, const char *s, size_t n)
{
	char *r = d;
	while (*d) d++;
	while (n-- && *s) *d++ = *s++;
	*d = 0;
	return r;
}

size_t strlcpy(char *d, const char *s, size_t n)
{
	size_t len = strlen(s);
	if (n) {
		size_t c = len >= n ? n - 1 : len;
		memcpy(d, s, c);
		d[c] = 0;
	}
	return len;
}

size_t strlcat(char *d, const char *s, size_t n)
{
	size_t dl = strnlen(d, n);
	if (dl == n) return n + strlen(s);
	return dl + strlcpy(d + dl, s, n - dl);
}

ssize_t sized_strscpy(char *d, const char *s, size_t n)
{
	if (n == 0) return -E2BIG;
	size_t i;
	for (i = 0; i < n; i++) {
		d[i] = s[i];
		if (!s[i]) return i;
	}
	d[n - 1] = 0;
	return -E2BIG;
}

ssize_t strscpy_pad(char *d, const char *s, size_t n)
{
	ssize_t r = sized_strscpy(d, s, n);
	size_t used = r < 0 ? n : (size_t)r + 1;
	if (used < n) memset(d + used, 0, n - used);
	return r;
}

int strcasecmp(const char *a, const char *b)
{
	int c1, c2;
	do {
		c1 = tolower(*a++);
		c2 = tolower(*b++);
	} while (c1 == c2 && c1 != 0);
	return c1 - c2;
}

int strncasecmp(const char *a, const char *b, size_t n)
{
	int c1 = 0, c2 = 0;
	while (n--) {
		c1 = tolower(*a++);
		c2 = tolower(*b++);
		if (c1 != c2 || !c1) break;
	}
	return c1 - c2;
}

char *strnchr(const char *s, size_t count, int c)
{
	while (count--) {
		if (*s == (char)c) return (char *)s;
		if (*s++ == '\0') break;
	}
	return NULL;
}

char *strchrnul(const char *s, int c)
{
	while (*s && *s != (char)c) s++;
	return (char *)s;
}

char *strnstr(const char *h, const char *n, size_t len)
{
	size_t l2 = strlen(n);
	if (!l2) return (char *)h;
	while (len >= l2) {
		len--;
		if (!memcmp(h, n, l2)) return (char *)h;
		h++;
	}
	return NULL;
}

char *strsep(char **s, const char *ct)
{
	char *sbegin = *s;
	if (!sbegin) return NULL;
	char *end = strpbrk(sbegin, ct);
	if (end) *end++ = '\0';
	*s = end;
	return sbegin;
}

char *skip_spaces(const char *s)
{
	while (isspace(*s)) s++;
	return (char *)s;
}

char *strim(char *s)
{
	size_t size = strlen(s);
	if (!size) return s;
	char *end = s + size - 1;
	while (end >= s && isspace(*end)) end--;
	*(end + 1) = '\0';
	return skip_spaces(s);
}

void *memchr_inv(const void *start, int c, size_t bytes)
{
	const u8 *p = start;
	for (size_t i = 0; i < bytes; i++)
		if (p[i] != (u8)c) return (void *)(p + i);
	return NULL;
}

void *memscan(void *addr, int c, size_t size)
{
	unsigned char *p = addr;
	while (size) {
		if (*p == (unsigned char)c) return p;
		p++;
		size--;
	}
	return p;
}

char *strreplace(char *s, char old, char new)
{
	for (; *s; ++s)
		if (*s == old) *s = new;
	return s;
}

bool sysfs_streq(const char *s1, const char *s2)
{
	while (*s1 && *s1 == *s2) {
		s1++;
		s2++;
	}
	if (*s1 == *s2) return true;
	if (!*s1 && *s2 == '\n' && !s2[1]) return true;
	if (*s1 == '\n' && !s1[1] && !*s2) return true;
	return false;
}

int match_string(const char *const *array, size_t n, const char *string)
{
	for (size_t index = 0; index < n; index++) {
		const char *item = array[index];
		if (!item) break;
		if (!strcmp(item, string)) return index;
	}
	return -EINVAL;
}

void *kmemdup(const void *src, size_t len, gfp_t gfp)
{
	void *p = kmalloc(len, gfp);
	if (p) memcpy(p, src, len);
	return p;
}

void *kmemdup_nul(const char *s, size_t len, gfp_t gfp)
{
	char *buf = kmalloc(len + 1, gfp);
	if (buf) {
		memcpy(buf, s, len);
		buf[len] = '\0';
	}
	return buf;
}

char *kstrdup(const char *s, gfp_t gfp)
{
	if (!s) return NULL;
	return kmemdup_nul(s, strlen(s), gfp);
}

const char *kstrdup_const(const char *s, gfp_t gfp) { return kstrdup(s, gfp); }

char *kstrndup(const char *s, size_t max, gfp_t gfp)
{
	if (!s) return NULL;
	return kmemdup_nul(s, strnlen(s, max), gfp);
}

void kfree_const(const void *x) { kfree(x); }


static unsigned int parse_base(const char **s, unsigned int base)
{
	const char *p = *s;
	if (base == 0) {
		if (p[0] == '0') {
			if (tolower(p[1]) == 'x' && isxdigit(p[2])) base = 16;
			else base = 8;
		} else {
			base = 10;
		}
	}
	if (base == 16 && p[0] == '0' && tolower(p[1]) == 'x') p += 2;
	*s = p;
	return base;
}

static int parse_ull(const char *s, unsigned int base, unsigned long long *res, const char **end)
{
	base = parse_base(&s, base);
	unsigned long long v = 0;
	int n = 0;
	for (;; s++, n++) {
		unsigned int d;
		if (*s >= '0' && *s <= '9') d = *s - '0';
		else if (tolower(*s) >= 'a' && tolower(*s) <= 'f') d = tolower(*s) - 'a' + 10;
		else break;
		if (d >= base) break;
		if (v > (ULLONG_MAX - d) / base) return -ERANGE;
		v = v * base + d;
	}
	if (end) *end = s;
	*res = v;
	return n ? 0 : -EINVAL;
}

int _kstrtoull(const char *s, unsigned int base, unsigned long long *res)
{
	const char *end;
	if (*s == '+') s++;
	int r = parse_ull(s, base, res, &end);
	if (r) return r;
	if (*end == '\n') end++;
	return *end ? -EINVAL : 0;
}

int kstrtoull(const char *s, unsigned int base, unsigned long long *res) { return _kstrtoull(s, base, res); }

int kstrtoll(const char *s, unsigned int base, long long *res)
{
	unsigned long long tmp;
	int rv;
	if (s[0] == '-') {
		rv = _kstrtoull(s + 1, base, &tmp);
		if (rv < 0) return rv;
		if ((long long)-tmp > 0) return -ERANGE;
		*res = -tmp;
	} else {
		rv = kstrtoull(s, base, &tmp);
		if (rv < 0) return rv;
		if ((long long)tmp < 0) return -ERANGE;
		*res = tmp;
	}
	return 0;
}

int _kstrtoul(const char *s, unsigned int base, unsigned long *res)
{
	unsigned long long tmp;
	int rv = kstrtoull(s, base, &tmp);
	if (rv < 0) return rv;
	*res = tmp;
	return 0;
}

int _kstrtol(const char *s, unsigned int base, long *res)
{
	long long tmp;
	int rv = kstrtoll(s, base, &tmp);
	if (rv < 0) return rv;
	*res = tmp;
	return 0;
}

#define LKPI_KSTRTO(name, type, via, viatype, minv, maxv) \
int name(const char *s, unsigned int base, type *res) \
{ \
	viatype tmp; \
	int rv = via(s, base, &tmp); \
	if (rv < 0) return rv; \
	if (tmp < (viatype)(minv) || tmp > (viatype)(maxv)) return -ERANGE; \
	*res = tmp; \
	return 0; \
}
LKPI_KSTRTO(kstrtouint, unsigned int, kstrtoull, unsigned long long, 0, UINT_MAX)
LKPI_KSTRTO(kstrtoint, int, kstrtoll, long long, INT_MIN, INT_MAX)
LKPI_KSTRTO(kstrtou16, u16, kstrtoull, unsigned long long, 0, U16_MAX)
LKPI_KSTRTO(kstrtos16, s16, kstrtoll, long long, S16_MIN, S16_MAX)
LKPI_KSTRTO(kstrtou8, u8, kstrtoull, unsigned long long, 0, U8_MAX)
LKPI_KSTRTO(kstrtos8, s8, kstrtoll, long long, S8_MIN, S8_MAX)

int kstrtobool(const char *s, bool *res)
{
	if (!s) return -EINVAL;
	switch (s[0]) {
	case 'y': case 'Y': case 't': case 'T': case '1':
		*res = true;
		return 0;
	case 'n': case 'N': case 'f': case 'F': case '0':
		*res = false;
		return 0;
	case 'o': case 'O':
		if (tolower(s[1]) == 'n') { *res = true; return 0; }
		if (tolower(s[1]) == 'f') { *res = false; return 0; }
		break;
	}
	return -EINVAL;
}

int kstrtoint_from_user(const char __user *s, size_t count, unsigned int base, int *res)
{
	char buf[32];
	count = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, s, count)) return -EFAULT;
	buf[count] = 0;
	return kstrtoint(buf, base, res);
}

unsigned long long simple_strtoull(const char *cp, char **endp, unsigned int base)
{
	unsigned long long v = 0;
	const char *end = cp;
	parse_ull(cp, base, &v, &end);
	if (endp) *endp = (char *)end;
	return v;
}

unsigned long simple_strtoul(const char *cp, char **endp, unsigned int base) { return simple_strtoull(cp, endp, base); }

long long simple_strtoll(const char *cp, char **endp, unsigned int base)
{
	if (*cp == '-') return -(long long)simple_strtoull(cp + 1, endp, base);
	return simple_strtoull(cp, endp, base);
}

long simple_strtol(const char *cp, char **endp, unsigned int base) { return simple_strtoll(cp, endp, base); }

int get_option(char **str, int *pint)
{
	char *cur = *str;
	if (!cur || !(*cur)) return 0;
	if (*cur == '-') *pint = -simple_strtol(++cur, str, 0);
	else *pint = simple_strtol(cur, str, 0);
	if (cur == *str) return 0;
	if (**str == ',') {
		(*str)++;
		return 2;
	}
	if (**str == '-') return 3;
	return 1;
}

char *get_options(const char *str, int nints, int *ints)
{
	int i = 1;
	char *s = (char *)str;
	while (i < nints) {
		int res = get_option(&s, ints + i);
		if (res == 0) break;
		i++;
		if (res == 1) break;
	}
	ints[0] = i - 1;
	return s;
}

unsigned long long memparse(const char *ptr, char **retptr)
{
	char *endptr;
	unsigned long long ret = simple_strtoull(ptr, &endptr, 0);
	switch (*endptr) {
	case 'E': case 'e': ret <<= 10; fallthrough;
	case 'P': case 'p': ret <<= 10; fallthrough;
	case 'T': case 't': ret <<= 10; fallthrough;
	case 'G': case 'g': ret <<= 10; fallthrough;
	case 'M': case 'm': ret <<= 10; fallthrough;
	case 'K': case 'k': ret <<= 10; endptr++; fallthrough;
	default: break;
	}
	if (retptr) *retptr = endptr;
	return ret;
}

int vsscanf(const char *buf, const char *fmt, va_list args)
{
	const char *s = buf;
	int n = 0;
	while (*fmt) {
		if (isspace(*fmt)) {
			fmt++;
			while (isspace(*s)) s++;
			continue;
		}
		if (*fmt != '%') {
			if (*fmt++ != *s++) break;
			continue;
		}
		fmt++;
		int lng = 0;
		while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') { if (*fmt == 'l') lng++; fmt++; }
		while (isspace(*s)) s++;
		char conv = *fmt++;
		if (conv == 'd' || conv == 'u' || conv == 'x' || conv == 'i') {
			const char *e;
			bool neg = false;
			if (*s == '-') { neg = true; s++; }
			unsigned long long v;
			if (parse_ull(s, conv == 'x' ? 16 : conv == 'i' ? 0 : 10, &v, &e)) break;
			s = e;
			long long sv = neg ? -(long long)v : (long long)v;
			if (lng >= 2) *va_arg(args, long long *) = sv;
			else if (lng == 1) *va_arg(args, long *) = sv;
			else *va_arg(args, int *) = (int)sv;
			n++;
		} else if (conv == 's') {
			char *d = va_arg(args, char *);
			while (*s && !isspace(*s)) *d++ = *s++;
			*d = 0;
			n++;
		} else if (conv == 'c') {
			*va_arg(args, char *) = *s++;
			n++;
		} else {
			break;
		}
	}
	return n;
}

int sscanf(const char *buf, const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	int r = vsscanf(buf, fmt, va);
	va_end(va);
	return r;
}

const unsigned char _ctype[] = {
_C,_C,_C,_C,_C,_C,_C,_C,
_C,_C|_S,_C|_S,_C|_S,_C|_S,_C|_S,_C,_C,
_C,_C,_C,_C,_C,_C,_C,_C,
_C,_C,_C,_C,_C,_C,_C,_C,
_S|_SP,_P,_P,_P,_P,_P,_P,_P,
_P,_P,_P,_P,_P,_P,_P,_P,
_D,_D,_D,_D,_D,_D,_D,_D,
_D,_D,_P,_P,_P,_P,_P,_P,
_P,_U|_X,_U|_X,_U|_X,_U|_X,_U|_X,_U|_X,_U,
_U,_U,_U,_U,_U,_U,_U,_U,
_U,_U,_U,_U,_U,_U,_U,_U,
_U,_U,_U,_P,_P,_P,_P,_P,
_P,_L|_X,_L|_X,_L|_X,_L|_X,_L|_X,_L|_X,_L,
_L,_L,_L,_L,_L,_L,_L,_L,
_L,_L,_L,_L,_L,_L,_L,_L,
_L,_L,_L,_P,_P,_P,_P,_C,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
_S|_SP,_P,_P,_P,_P,_P,_P,_P,_P,_P,_P,_P,_P,_P,_P,_P,
_P,_P,_P,_P,_P,_P,_P,_P,_P,_P,_P,_P,_P,_P,_P,_P,
_U,_U,_U,_U,_U,_U,_U,_U,_U,_U,_U,_U,_U,_U,_U,_U,
_U,_U,_U,_U,_U,_U,_U,_P,_U,_U,_U,_U,_U,_U,_U,_L,
_L,_L,_L,_L,_L,_L,_L,_L,_L,_L,_L,_L,_L,_L,_L,_L,
_L,_L,_L,_L,_L,_L,_L,_P,_L,_L,_L,_L,_L,_L,_L,_L};

const char hex_asc[] = "0123456789abcdef";
const char hex_asc_upper[] = "0123456789ABCDEF";

int hex_to_bin(unsigned char ch)
{
	unsigned char cu = ch & 0xdf;
	return -1 + ((ch - '0' + 1) & (unsigned)((ch - '9' - 1) & ('0' - 1 - ch)) >> 8) +
	       ((cu - 'A' + 11) & (unsigned)((cu - 'F' - 1) & ('A' - 1 - cu)) >> 8);
}

int hex2bin(u8 *dst, const char *src, size_t count)
{
	while (count--) {
		int hi = hex_to_bin(*src++);
		if (unlikely(hi < 0)) return -EINVAL;
		int lo = hex_to_bin(*src++);
		if (unlikely(lo < 0)) return -EINVAL;
		*dst++ = (hi << 4) | lo;
	}
	return 0;
}

char *bin2hex(char *dst, const void *src, size_t count)
{
	const unsigned char *_src = src;
	while (count--) dst = hex_byte_pack(dst, *_src++);
	return dst;
}


unsigned long find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset)
{
	while (offset < size) {
		unsigned long w = addr[offset / BITS_PER_LONG] >> (offset % BITS_PER_LONG);
		if (w) {
			offset += __ffs(w);
			return offset < size ? offset : size;
		}
		offset = (offset / BITS_PER_LONG + 1) * BITS_PER_LONG;
	}
	return size;
}

unsigned long find_next_zero_bit(const unsigned long *addr, unsigned long size, unsigned long offset)
{
	while (offset < size) {
		unsigned long w = ~addr[offset / BITS_PER_LONG] >> (offset % BITS_PER_LONG);
		if (w) {
			offset += __ffs(w);
			return offset < size ? offset : size;
		}
		offset = (offset / BITS_PER_LONG + 1) * BITS_PER_LONG;
	}
	return size;
}

unsigned long find_next_and_bit(const unsigned long *a, const unsigned long *b, unsigned long size, unsigned long offset)
{
	for (; offset < size; offset++)
		if (test_bit(offset, a) && test_bit(offset, b)) return offset;
	return size;
}

unsigned long find_first_bit(const unsigned long *addr, unsigned long size) { return find_next_bit(addr, size, 0); }
unsigned long find_first_zero_bit(const unsigned long *addr, unsigned long size) { return find_next_zero_bit(addr, size, 0); }

unsigned long find_last_bit(const unsigned long *addr, unsigned long size)
{
	while (size--)
		if (test_bit(size, addr)) return size;
	return size;
}

void bitmap_set(unsigned long *map, unsigned int start, unsigned int nbits)
{
	for (unsigned int i = 0; i < nbits; i++) __set_bit(start + i, map);
}

void bitmap_clear(unsigned long *map, unsigned int start, unsigned int nbits)
{
	for (unsigned int i = 0; i < nbits; i++) __clear_bit(start + i, map);
}

unsigned int bitmap_weight(const unsigned long *src, unsigned int nbits)
{
	unsigned int w = 0;
	for (unsigned int i = 0; i < nbits / BITS_PER_LONG; i++) w += hweight_long(src[i]);
	if (nbits % BITS_PER_LONG) w += hweight_long(src[nbits / BITS_PER_LONG] & BITMAP_LAST_WORD_MASK(nbits));
	return w;
}

bool bitmap_empty(const unsigned long *src, unsigned int nbits) { return find_first_bit(src, nbits) == nbits; }
bool bitmap_full(const unsigned long *src, unsigned int nbits) { return find_first_zero_bit(src, nbits) == nbits; }

bool bitmap_equal(const unsigned long *a, const unsigned long *b, unsigned int nbits)
{
	for (unsigned int i = 0; i < nbits; i++)
		if (test_bit(i, a) != test_bit(i, b)) return false;
	return true;
}

bool bitmap_intersects(const unsigned long *a, const unsigned long *b, unsigned int nbits)
{
	for (unsigned int k = 0; k < BITS_TO_LONGS(nbits); k++) {
		unsigned long m = (k == nbits / BITS_PER_LONG) ? BITMAP_LAST_WORD_MASK(nbits) : ~0UL;
		if (a[k] & b[k] & m) return true;
	}
	return false;
}

bool bitmap_subset(const unsigned long *a, const unsigned long *b, unsigned int nbits)
{
	for (unsigned int i = 0; i < nbits; i++)
		if (test_bit(i, a) && !test_bit(i, b)) return false;
	return true;
}

bool bitmap_and(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned int nbits)
{
	unsigned long r = 0;
	for (unsigned int k = 0; k < BITS_TO_LONGS(nbits); k++) r |= (dst[k] = a[k] & b[k]);
	return r != 0;
}

void bitmap_or(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned int nbits)
{
	for (unsigned int k = 0; k < BITS_TO_LONGS(nbits); k++) dst[k] = a[k] | b[k];
}

void bitmap_xor(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned int nbits)
{
	for (unsigned int k = 0; k < BITS_TO_LONGS(nbits); k++) dst[k] = a[k] ^ b[k];
}

bool bitmap_andnot(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned int nbits)
{
	unsigned long r = 0;
	for (unsigned int k = 0; k < BITS_TO_LONGS(nbits); k++) r |= (dst[k] = a[k] & ~b[k]);
	return r != 0;
}

void bitmap_complement(unsigned long *dst, const unsigned long *src, unsigned int nbits)
{
	for (unsigned int k = 0; k < BITS_TO_LONGS(nbits); k++) dst[k] = ~src[k];
}

void bitmap_shift_right(unsigned long *dst, const unsigned long *src, unsigned int shift, unsigned int nbits)
{
	for (unsigned int i = 0; i < nbits; i++) {
		if (i + shift < nbits && test_bit(i + shift, src)) __set_bit(i, dst);
		else __clear_bit(i, dst);
	}
}

void bitmap_shift_left(unsigned long *dst, const unsigned long *src, unsigned int shift, unsigned int nbits)
{
	for (int i = (int)nbits - 1; i >= 0; i--) {
		if (i >= (int)shift && test_bit(i - shift, src)) __set_bit(i, dst);
		else __clear_bit(i, dst);
	}
}

unsigned long bitmap_find_next_zero_area_off(unsigned long *map, unsigned long size, unsigned long start,
					     unsigned int nr, unsigned long align_mask, unsigned long align_offset)
{
	unsigned long index, end, i;
again:
	index = find_next_zero_bit(map, size, start);
	index = ((index + align_offset + align_mask) & ~align_mask) - align_offset;
	end = index + nr;
	if (end > size) return end;
	i = find_next_bit(map, end, index);
	if (i < end) {
		start = i + 1;
		goto again;
	}
	return index;
}

int bitmap_find_free_region(unsigned long *bitmap, unsigned int bits, int order)
{
	unsigned int pos, end;
	for (pos = 0; (end = pos + (1U << order)) <= bits; pos = end) {
		if (find_next_bit(bitmap, end, pos) < end) continue;
		bitmap_set(bitmap, pos, 1U << order);
		return pos;
	}
	return -ENOMEM;
}

void bitmap_release_region(unsigned long *bitmap, unsigned int pos, int order) { bitmap_clear(bitmap, pos, 1U << order); }

int bitmap_allocate_region(unsigned long *bitmap, unsigned int pos, int order)
{
	unsigned int len = 1U << order;
	if (find_next_bit(bitmap, pos + len, pos) < pos + len) return -EBUSY;
	bitmap_set(bitmap, pos, len);
	return 0;
}

unsigned long *bitmap_alloc(unsigned int nbits, gfp_t flags) { return kmalloc_array(BITS_TO_LONGS(nbits), sizeof(unsigned long), flags); }
unsigned long *bitmap_zalloc(unsigned int nbits, gfp_t flags) { return bitmap_alloc(nbits, flags | __GFP_ZERO); }
void bitmap_free(const unsigned long *bitmap) { kfree(bitmap); }


unsigned long int_sqrt(unsigned long x)
{
	unsigned long b, m, y = 0;
	if (x <= 1) return x;
	m = 1UL << (__fls(x) & ~1UL);
	while (m != 0) {
		b = y + m;
		y >>= 1;
		if (x >= b) {
			x -= b;
			y += m;
		}
		m >>= 2;
	}
	return y;
}


u64 int_pow(u64 base, unsigned int exp)
{
	u64 result = 1;
	while (exp) {
		if (exp & 1) result *= base;
		exp >>= 1;
		base *= base;
	}
	return result;
}

unsigned long gcd(unsigned long a, unsigned long b)
{
	while (b) {
		unsigned long t = a % b;
		a = b;
		b = t;
	}
	return a;
}

unsigned long lcm(unsigned long a, unsigned long b) { return (a && b) ? (a / gcd(a, b)) * b : 0; }

void rational_best_approximation(unsigned long given_numerator, unsigned long given_denominator,
				 unsigned long max_numerator, unsigned long max_denominator,
				 unsigned long *best_numerator, unsigned long *best_denominator)
{
	unsigned long n, d, n0, d0, n1, d1, n2, d2;
	n = given_numerator;
	d = given_denominator;
	n0 = d1 = 0;
	n1 = d0 = 1;
	for (;;) {
		unsigned long dp, a;
		if (d == 0) break;
		dp = d;
		a = n / d;
		d = n % d;
		n = dp;
		n2 = n0 + a * n1;
		d2 = d0 + a * d1;
		if ((n2 > max_numerator) || (d2 > max_denominator)) {
			unsigned long t = ULONG_MAX;
			if (d1) t = (max_denominator - d0) / d1;
			if (n1) t = min(t, (max_numerator - n0) / n1);
			if (!d1 || 2u * t > a || (2u * t == a && d0 * dp > d1 * d)) {
				n1 = n0 + t * n1;
				d1 = d0 + t * d1;
			}
			break;
		}
		n0 = n1;
		n1 = n2;
		d0 = d1;
		d1 = d2;
	}
	*best_numerator = n1;
	*best_denominator = d1;
}

static u64 rnd_state = 0x9E3779B97F4A7C15ULL;

u32 lkpi_prandom_u32(void)
{
	u64 s = READ_ONCE(rnd_state);
	u32 lo, hi;
	asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
	s ^= ((u64)hi << 32 | lo) * 0xBF58476D1CE4E5B9ULL;
	s ^= s << 13;
	s ^= s >> 7;
	s ^= s << 17;
	WRITE_ONCE(rnd_state, s);
	return (u32)(s >> 16);
}

void get_random_bytes(void *buf, size_t len)
{
	u8 *p = buf;
	while (len) {
		u32 v = lkpi_prandom_u32();
		size_t n = min(len, sizeof(v));
		memcpy(p, &v, n);
		p += n;
		len -= n;
	}
}


int seq_buf_printf(struct seq_buf *s, const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	size_t room = s->len < s->size ? s->size - s->len : 0;
	int n = lkpi_vsnprintf(s->buffer + (s->len < s->size ? s->len : 0), room, fmt, va);
	va_end(va);
	s->len += n;
	return n < (int)room ? 0 : -1;
}

int seq_buf_puts(struct seq_buf *s, const char *str) { return seq_buf_printf(s, "%s", str); }
