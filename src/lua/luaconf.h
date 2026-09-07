#ifndef luaconf_h
#define luaconf_h

#include <limits.h>
#include <stddef.h>

/* =========================================================
** LuOS bare-metal configuration — Lua 5.5.1
** No POSIX, no DLL, no dynamic linking, no locale
** ========================================================= */

/* Disable all platform-specific backends */
#undef LUA_USE_POSIX
#undef LUA_USE_DLOPEN
#undef LUA_USE_WINDOWS
#undef LUA_DL_DLL
#undef LUA_USE_LINUX
#undef LUA_USE_MACOSX
#undef LUA_USE_IOS

/* Integer size detection */
#define LUAI_IS32INT	((UINT_MAX >> 30) >= 3)

#define LUA_INT_INT		1
#define LUA_INT_LONG		2
#define LUA_INT_LONGLONG	3

#define LUA_FLOAT_FLOAT		1
#define LUA_FLOAT_DOUBLE	2
#define LUA_FLOAT_LONGDOUBLE	3

/* Use 64-bit integers and double floats */
#define LUA_INT_DEFAULT		LUA_INT_LONGLONG
#define LUA_FLOAT_DEFAULT	LUA_FLOAT_DOUBLE

#define LUA_32BITS	0
#define LUA_C89_NUMBERS	0

#define LUA_INT_TYPE	LUA_INT_DEFAULT
#define LUA_FLOAT_TYPE	LUA_FLOAT_DEFAULT

/* Path separators (unused in kernel, but required by headers) */
#define LUA_PATH_SEP		";"
#define LUA_PATH_MARK		"?"
#define LUA_EXEC_DIR		"!"
#define LUA_VDIR	LUA_VERSION_MAJOR "." LUA_VERSION_MINOR

#define LUA_ROOT		"/lua/"
#define LUA_LDIR		LUA_ROOT "lua/"
#define LUA_CDIR		LUA_ROOT
#define LUA_PATH_DEFAULT	""
#define LUA_CPATH_DEFAULT	""

/* API visibility */
#define LUA_API		extern
#define LUALIB_API	extern
#define LUAMOD_API	extern

/* Mark internal functions — in LuOS we expose everything as extern */
#undef LUAI_FUNC
#define LUAI_FUNC	extern
#define LUAI_DDEF	/* empty */
#define LUAI_DDEC(def)	extern def

/* Integer type — use long long */
#if defined(__GNUC__)
#define LUA_INTEGER		long long
#define LUA_INTEGER_FRMLEN	"ll"
#define LUA_MAXINTEGER		LLONG_MAX
#define LUA_MININTEGER		LLONG_MIN
#define LUA_MAXUNSIGNED		ULLONG_MAX
#else
#error "Need GCC for LuOS"
#endif

/* Float type */
#define LUA_NUMBER		double
#define LUA_NUMBER_FRMLEN	""
#define LUA_NUMBER_FMT		"%.14g"
#define LUAI_NUMUNM(L,a)	(-(a))
#define lua_Number2str(s,sz,n)	l_sprintf((s), sz, LUA_NUMBER_FMT, (LUAI_UACNUMBER)(n))
#define LUAI_UACNUMBER		double
#define LUAI_UACINT		long long
#define lua_str2number(s,p)	strtod((s), (p))

/* Unsigned integer */
#define LUA_UNSIGNED		unsigned long long

/* Continuation contexts */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#include <stdint.h>
#if defined(INTPTR_MAX)
#define LUA_KCONTEXT	intptr_t
#else
#define LUA_KCONTEXT	ptrdiff_t
#endif
#else
#define LUA_KCONTEXT	ptrdiff_t
#endif

/* String formatting helpers */
#define l_sprintf(s,sz,f,i)	snprintf(s,sz,f,i)
#define lua_strx2number(s,p)	lua_str2number(s,p)
#define lua_pointer2str(buff,sz,p)	l_sprintf(buff,sz,"%p",p)
#define lua_number2strx(L,b,sz,f,n)	((void)L, l_sprintf(b,sz,f,(LUAI_UACNUMBER)(n)))

/* Locale: bare metal has no locale, always use '.' as decimal point */
#define lua_getlocaledecpoint()		('.')

/* Branch prediction hints (GCC) */
#if defined(__GNUC__) && !defined(LUA_NOBUILTIN)
#define luai_likely(x)		(__builtin_expect(((x) != 0), 1))
#define luai_unlikely(x)	(__builtin_expect(((x) != 0), 0))
#else
#define luai_likely(x)		(x)
#define luai_unlikely(x)	(x)
#endif

#if defined(LUA_CORE) || defined(LUA_LIB)
#define l_likely(x)	luai_likely(x)
#define l_unlikely(x)	luai_unlikely(x)
#endif

/* Stack size */
#if LUAI_IS32INT
#define LUAI_MAXSTACK		1000000
#else
#define LUAI_MAXSTACK		15000
#endif

/* Extra space per lua_State */
#define LUA_EXTRASPACE		(sizeof(void *))

/* Source identifier truncation */
#define LUA_IDSIZE		60

/* Buffer size for lauxlib */
#define LUAL_BUFFERSIZE		((int)(16 * sizeof(void*) * sizeof(lua_Number)))

/* Alignment */
#define LUAI_MAXALIGN	lua_Number n; double u; void *s; lua_Integer i; long l

/* LuOS marker */
#define LUA_USE_LUOS	1

/* Seed for string hashing — use a fixed seed since we have no time/rand */
#define luai_makeseed()	((unsigned int)0xDEADBEEF)

/*
** Standard math operation macros for double (LUA_FLOAT_DOUBLE).
** l_mathop(op) appends no suffix — standard C double functions.
** l_floatatt(x) expands to the DBL_x constant from <float.h>.
** l_floor(x) wraps floor() for use without the l_mathop indirection.
*/
#include <math.h>
#include <float.h>
#if !defined(l_mathop)
#define l_mathop(op)    op
#endif
#if !defined(l_floatatt)
#define l_floatatt(x)   (DBL_##x)
#endif
#if !defined(l_floor)
#define l_floor(x)      floor(x)
#endif

/* Integer format string (uses LUA_INTEGER_FRMLEN defined above) */
#if !defined(LUA_INTEGER_FMT)
#define LUA_INTEGER_FMT		("%" LUA_INTEGER_FRMLEN "d")
#endif

/* Full-precision float format: enough digits to round-trip through str->num */
#if !defined(LUA_NUMBER_FMT_N)
#define LUA_NUMBER_FMT_N	"%." LUA_NUMBER_FRMLEN "g"
#endif

/* Convert a lua_Integer to a string buffer */
#if !defined(lua_integer2str)
#define lua_integer2str(s,sz,n)      l_sprintf((s), sz, LUA_INTEGER_FMT, (LUAI_UACINT)(n))
#endif

#endif /* luaconf_h */