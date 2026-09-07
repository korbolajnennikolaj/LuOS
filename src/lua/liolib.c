/*
** liolib.c — the Lua "io" library for LuOS.
**
** Real disk I/O against the kernel's single root filesystem mount (see
** kernel/rootfs.h) — the same "/" the kernel shell's mount/ls/cat use
** and MicroPython's os module/open() use. This follows upstream Lua
** 5.x's liolib.c structure (file objects as LUA_FILEHANDLE userdata
** wrapping a luaL_Stream, default input/output files, io.lines()/
** file:lines() iterators, read format specifiers "l"/"L"/"a"/"n") on
** top of this port's FILE* shim (luos_file.h) as the underlying stdio
** implementation. If nothing is mounted at "/", io.open() reports that
** as an ordinary (nil, message, errno) failure via luaL_fileresult(),
** exactly the way a real fopen() failure would - no separate "is a
** filesystem mounted" check needed on the Lua side, fopen()/ENODEV
** already covers it.
*/

#define LUA_LIB

#include "lprefix.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luos_file.h"

/* Fixed registry slots for the current default input/output files,
 * simpler than upstream's per-closure upvalue scheme - there's no
 * coroutine-local multiplexing to worry about on this single-threaded
 * kernel.
 *
 * IMPORTANT: must NOT collide with Lua's own reserved registry integer
 * indices (LUA_RIDX_GLOBALS=2, LUA_RIDX_MAINTHREAD=3, LUA_RIDX_LAST=3 -
 * see lua.h). Using values well clear of LUA_RIDX_LAST avoids ever
 * clobbering the real globals table reference. */
#define IO_INPUT_SLOT   100
#define IO_OUTPUT_SLOT  101

/* ------------------------------------------------------------------
** File handle creation/closing helpers
** ------------------------------------------------------------------ */

static int io_fclose (lua_State *L) {
    luaL_Stream *p = (luaL_Stream *)luaL_checkudata(L, 1, LUA_FILEHANDLE);
    int res = fclose(p->f);
    p->closef = NULL;
    return luaL_fileresult(L, (res == 0), NULL);
}

static luaL_Stream *newprefile (lua_State *L) {
    luaL_Stream *p = (luaL_Stream *)lua_newuserdatauv(L, sizeof(luaL_Stream), 0);
    p->f = NULL;
    p->closef = NULL;  /* mark file handle as 'closed' until fully initialized */
    luaL_setmetatable(L, LUA_FILEHANDLE);
    return p;
}

static int io_noclose (lua_State *L) {
    luaL_checkudata(L, 1, LUA_FILEHANDLE);
    lua_pushnil(L);
    lua_pushliteral(L, "cannot close standard file");
    return 2;
}

static void register_stdfile (lua_State *L, FILE *f, const char *name, int slot) {
    luaL_Stream *p = newprefile(L);
    p->f = f;
    p->closef = &io_noclose;
    lua_setfield(L, -2, name);       /* iolib[name] = the file object */
    if (slot) {
        lua_getfield(L, -1, name);
        lua_rawseti(L, LUA_REGISTRYINDEX, slot);
    }
}

/* ------------------------------------------------------------------
** io.open / io.close / io.input / io.output
** ------------------------------------------------------------------ */

static int io_open (lua_State *L) {
    const char *filename = luaL_checkstring(L, 1);
    const char *mode = luaL_optstring(L, 2, "r");
    luaL_Stream *p = newprefile(L);
    p->f = fopen(filename, mode);
    if (p->f == NULL) {
        return luaL_fileresult(L, 0, filename);
    }
    p->closef = &io_fclose;
    return 1;
}

static luaL_Stream *getiofile (lua_State *L, int slot) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, slot);
    luaL_Stream *p = (luaL_Stream *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return p;
}

static int g_iofile (lua_State *L, int slot, const char *mode) {
    if (!lua_isnoneornil(L, 1)) {
        const char *filename = lua_tostring(L, 1);
        if (filename) {
            luaL_Stream *p = newprefile(L);
            p->f = fopen(filename, mode);
            if (p->f == NULL) return luaL_fileresult(L, 0, filename);
            p->closef = &io_fclose;
        }
        else {
            luaL_checkudata(L, 1, LUA_FILEHANDLE);
            lua_pushvalue(L, 1);
        }
        lua_rawseti(L, LUA_REGISTRYINDEX, slot);
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, slot);
    return 1;
}

static int io_input (lua_State *L) { return g_iofile(L, IO_INPUT_SLOT, "r"); }
static int io_output (lua_State *L) { return g_iofile(L, IO_OUTPUT_SLOT, "w"); }

static int io_close (lua_State *L) {
    if (lua_isnone(L, 1)) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, IO_OUTPUT_SLOT);
    }
    luaL_checkudata(L, 1, LUA_FILEHANDLE);
    return io_fclose(L);
}

static int io_tmpfile (lua_State *L) {
    lua_pushnil(L);
    lua_pushliteral(L, "io.tmpfile not available");
    return 2;
}

static int io_type (lua_State *L) {
    luaL_checkany(L, 1);
    void *ud = luaL_testudata(L, 1, LUA_FILEHANDLE);
    if (ud == NULL) {
        lua_pushnil(L);
    }
    else if (((luaL_Stream *)ud)->closef == NULL) {
        lua_pushliteral(L, "closed file");
    }
    else {
        lua_pushliteral(L, "file");
    }
    return 1;
}

static int io_flush (lua_State *L) {
    (void)L;
    return 0;
}

static int io_popen (lua_State *L) {
    (void)L;
    lua_pushnil(L);
    lua_pushliteral(L, "io.popen not available");
    return 2;
}

/* ------------------------------------------------------------------
** Reading: format specifiers "l"/"*l" (a line, no newline), "L"/"*L"
** (a line, with newline), "a"/"*a" (rest of file), "n"/"*n" (a
** number). A plain integer argument reads that many bytes.
** ------------------------------------------------------------------ */

static int read_number (lua_State *L, FILE *f) {
    char buf[64];
    size_t n = 0;
    int c;
    do { c = fgetc(f); } while (c == ' ' || c == '\t' || c == '\n' || c == '\r');
    while (n < sizeof(buf) - 1 &&
           (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9') ||
            c == 'e' || c == 'E' || c == 'x' || c == 'X' ||
            (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
        buf[n++] = (char)c;
        c = fgetc(f);
    }
    if (c != EOF) ungetc(c, f);
    buf[n] = '\0';
    if (n == 0) { lua_pushnil(L); return 0; }
    char *endp = NULL;
    double d = strtod(buf, &endp);
    if (endp == buf) { lua_pushnil(L); return 0; }
    lua_pushnumber(L, (lua_Number)d);
    return 1;
}

/* Reads one line into a Lua string on top of the stack. Returns 1 if
 * anything was read (including an empty line before EOF), 0 on
 * immediate EOF with nothing read at all. */
static int read_line (lua_State *L, FILE *f, int chop) {
    luaL_Buffer b;
    int c;
    int any = 0;
    luaL_buffinit(L, &b);
    for (;;) {
        c = fgetc(f);
        if (c == EOF) break;
        any = 1;
        if (c == '\n') {
            if (!chop) luaL_addchar(&b, (char)c);
            break;
        }
        luaL_addchar(&b, (char)c);
    }
    if (!any) { luaL_pushresult(&b); lua_pop(L, 1); return 0; }
    luaL_pushresult(&b);
    return 1;
}

static int read_all (lua_State *L, FILE *f) {
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (;;) {
        char *p = luaL_prepbuffsize(&b, 512);
        size_t n = fread(p, 1, 512, f);
        luaL_addsize(&b, n);
        if (n < 512) break;
    }
    luaL_pushresult(&b);
    return 1;
}

static int read_chars (lua_State *L, FILE *f, size_t n) {
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    char *p = luaL_prepbuffsize(&b, n);
    size_t nr = fread(p, 1, n, f);
    luaL_addsize(&b, nr);
    luaL_pushresult(&b);
    return (n == 0) || (nr > 0);
}

/* Reads according to the arguments starting at stack index `first`.
 * With no format arguments at all, reads one line (chopped). Returns
 * the number of results pushed. */
static int g_read (lua_State *L, FILE *f, int first) {
    int nresults = 0;
    int nargs = lua_gettop(L) - (first - 1);
    if (nargs <= 0) {
        int ok = read_line(L, f, 1);
        if (!ok) lua_pushnil(L);
        return 1;
    }
    for (int i = first; i < first + nargs; i++) {
        int ok;
        if (lua_type(L, i) == LUA_TNUMBER) {
            size_t want = (size_t)lua_tointeger(L, i);
            ok = read_chars(L, f, want);
        }
        else {
            const char *p = lua_tostring(L, i);
            if (!p) return luaL_argerror(L, i, "invalid format");
            if (p[0] == '*') p++;  /* accept the legacy "*l" style too */
            switch (p[0]) {
                case 'n': ok = read_number(L, f); break;
                case 'l': ok = read_line(L, f, 1); break;
                case 'L': ok = read_line(L, f, 0); break;
                case 'a': ok = read_all(L, f); break;
                default: return luaL_argerror(L, i, "invalid format");
            }
        }
        if (!ok) {
            /* Match upstream behaviour: on failure to read this item,
             * push nil for it and stop (still return what's been read
             * so far, including this trailing nil). */
            lua_pushnil(L);
        }
        nresults++;
    }
    if (ferror(f)) return luaL_fileresult(L, 0, NULL);
    return nresults;
}

static int io_read (lua_State *L) {
    luaL_Stream *p = getiofile(L, IO_INPUT_SLOT);
    return g_read(L, p->f, 1);
}

static int f_read (lua_State *L) {
    luaL_Stream *p = (luaL_Stream *)luaL_checkudata(L, 1, LUA_FILEHANDLE);
    return g_read(L, p->f, 2);
}

/* ------------------------------------------------------------------
** Writing
** ------------------------------------------------------------------ */

static int g_write (lua_State *L, FILE *f, int arg) {
    int nargs = lua_gettop(L) - (arg - 1);
    int status = 1;
    for (; nargs > 0; nargs--, arg++) {
        if (lua_type(L, arg) == LUA_TNUMBER) {
            int len;
            if (lua_isinteger(L, arg)) {
                len = fprintf(f, LUA_INTEGER_FMT, (LUAI_UACINT)lua_tointeger(L, arg));
            }
            else {
                len = fprintf(f, LUA_NUMBER_FMT, (LUAI_UACNUMBER)lua_tonumber(L, arg));
            }
            status = status && (len > 0);
        }
        else {
            size_t l;
            const char *s = luaL_checklstring(L, arg, &l);
            status = status && (fwrite(s, sizeof(char), l, f) == l);
        }
    }
    if (status) return 1; /* the file/io object itself is already at the bottom, caller returns it */
    return luaL_fileresult(L, status, NULL);
}

static int io_write (lua_State *L) {
    luaL_Stream *p = getiofile(L, IO_OUTPUT_SLOT);
    int ok = g_write(L, p->f, 1);
    if (ok == 1) { lua_rawgeti(L, LUA_REGISTRYINDEX, IO_OUTPUT_SLOT); return 1; }
    return ok;
}

static int f_write (lua_State *L) {
    luaL_Stream *p = (luaL_Stream *)luaL_checkudata(L, 1, LUA_FILEHANDLE);
    int ok = g_write(L, p->f, 2);
    if (ok == 1) { lua_pushvalue(L, 1); return 1; }
    return ok;
}

/* ------------------------------------------------------------------
** Seeking
** ------------------------------------------------------------------ */

static const int seek_mode[] = { SEEK_SET, SEEK_CUR, SEEK_END };
static const char *const seek_modenames[] = { "set", "cur", "end", NULL };

static int f_seek (lua_State *L) {
    luaL_Stream *p = (luaL_Stream *)luaL_checkudata(L, 1, LUA_FILEHANDLE);
    int op = luaL_checkoption(L, 2, "cur", seek_modenames);
    lua_Integer offset = luaL_optinteger(L, 3, 0);
    int res = fseek(p->f, (long)offset, seek_mode[op]);
    if (res != 0) return luaL_fileresult(L, 0, NULL);
    lua_pushinteger(L, (lua_Integer)ftell(p->f));
    return 1;
}

static int f_setvbuf (lua_State *L) {
    luaL_checkudata(L, 1, LUA_FILEHANDLE);
    lua_pushboolean(L, 1);
    return 1;
}

/* ------------------------------------------------------------------
** file:close / :flush / __gc / __close / __tostring
** ------------------------------------------------------------------ */

static int f_close (lua_State *L) {
    luaL_Stream *p = (luaL_Stream *)luaL_checkudata(L, 1, LUA_FILEHANDLE);
    return (p->closef)(L);
}

static int f_flush (lua_State *L) {
    luaL_checkudata(L, 1, LUA_FILEHANDLE);
    lua_pushboolean(L, 1);
    return 1;
}

static int f_gc (lua_State *L) {
    luaL_Stream *p = (luaL_Stream *)luaL_checkudata(L, 1, LUA_FILEHANDLE);
    if (p->closef != NULL && p->closef != &io_noclose) io_fclose(L);
    return 0;
}

static int f_tostring (lua_State *L) {
    luaL_Stream *p = (luaL_Stream *)luaL_checkudata(L, 1, LUA_FILEHANDLE);
    if (p->closef == NULL) lua_pushliteral(L, "file (closed)");
    else lua_pushfstring(L, "file (%p)", (void *)p->f);
    return 1;
}

/* ------------------------------------------------------------------
** io.lines / file:lines
** ------------------------------------------------------------------ */

typedef struct LinesState {
    FILE *f;
    int close_at_end;
} LinesState;

static int lines_iter (lua_State *L) {
    LinesState *ls = (LinesState *)lua_touserdata(L, lua_upvalueindex(1));
    int ok = read_line(L, ls->f, 1);
    if (!ok) {
        lua_pushnil(L);
        if (ls->close_at_end) fclose(ls->f);
        return 1;
    }
    return 1;
}

static void make_lines_iter (lua_State *L, FILE *f, int close_at_end) {
    LinesState *ls = (LinesState *)lua_newuserdatauv(L, sizeof(LinesState), 0);
    ls->f = f;
    ls->close_at_end = close_at_end;
    lua_pushcclosure(L, lines_iter, 1);
}

static int f_lines (lua_State *L) {
    luaL_Stream *p = (luaL_Stream *)luaL_checkudata(L, 1, LUA_FILEHANDLE);
    make_lines_iter(L, p->f, 0);
    return 1;
}

static int io_lines (lua_State *L) {
    if (lua_isnoneornil(L, 1)) {
        luaL_Stream *p = getiofile(L, IO_INPUT_SLOT);
        make_lines_iter(L, p->f, 0);
        return 1;
    }
    const char *filename = luaL_checkstring(L, 1);
    FILE *f = fopen(filename, "r");
    if (f == NULL) return luaL_fileresult(L, 0, filename);
    make_lines_iter(L, f, 1);
    return 1;
}

/* ------------------------------------------------------------------
** Registration
** ------------------------------------------------------------------ */

static const luaL_Reg flib[] = {
    {"close", f_close},
    {"flush", f_flush},
    {"lines", f_lines},
    {"read", f_read},
    {"seek", f_seek},
    {"setvbuf", f_setvbuf},
    {"write", f_write},
    {"__gc", f_gc},
    {"__tostring", f_tostring},
    {NULL, NULL}
};

static void createmeta (lua_State *L) {
    luaL_newmetatable(L, LUA_FILEHANDLE);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, flib, 0);
    lua_pop(L, 1);
}

static const luaL_Reg iolib[] = {
    {"close",   io_close},
    {"flush",   io_flush},
    {"input",   io_input},
    {"lines",   io_lines},
    {"open",    io_open},
    {"output",  io_output},
    {"popen",   io_popen},
    {"read",    io_read},
    {"tmpfile", io_tmpfile},
    {"type",    io_type},
    {"write",   io_write},
    {NULL, NULL}
};

LUAMOD_API int luaopen_io (lua_State *L) {
    luaL_newlib(L, iolib);
    createmeta(L);

    register_stdfile(L, stdin, "stdin", IO_INPUT_SLOT);
    register_stdfile(L, stdout, "stdout", IO_OUTPUT_SLOT);
    register_stdfile(L, stderr, "stderr", 0);

    return 1;
}
