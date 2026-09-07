#define LUA_LIB

#include "lprefix.h"

#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static int os_clock (lua_State *L) {
    lua_pushnumber(L, (lua_Number)clock() / (lua_Number)CLOCKS_PER_SEC);
    return 1;
}

static int os_time (lua_State *L) {
    time_t t;
    if (lua_isnoneornil(L, 1))
        t = time(NULL);
    else {
        struct tm ts;
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_settop(L, 1);

        lua_getfield(L, 1, "year");  ts.tm_year = (int)luaL_checkinteger(L, -1) - 1900; lua_pop(L, 1);
        lua_getfield(L, 1, "month"); ts.tm_mon  = (int)luaL_checkinteger(L, -1) - 1;    lua_pop(L, 1);
        lua_getfield(L, 1, "day");   ts.tm_mday = (int)luaL_checkinteger(L, -1);        lua_pop(L, 1);

        lua_getfield(L, 1, "hour");
        ts.tm_hour = lua_isnoneornil(L, -1) ? 12 : (int)luaL_checkinteger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "min");
        ts.tm_min = lua_isnoneornil(L, -1) ? 0 : (int)luaL_checkinteger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "sec");
        ts.tm_sec = lua_isnoneornil(L, -1) ? 0 : (int)luaL_checkinteger(L, -1);
        lua_pop(L, 1);

        ts.tm_isdst = -1;
        t = mktime(&ts);
    }
    if (t == (time_t)-1)
        return luaL_error(L, "time result cannot be represented in this installation");
    lua_pushinteger(L, (lua_Integer)t);
    return 1;
}

static int os_date (lua_State *L) {
    const char *s = luaL_optstring(L, 1, "%c");
    time_t t = luaL_opt(L, (time_t)luaL_checkinteger, 2, time(NULL));
    struct tm tmr;
    struct tm *stm;

    if (*s == '!') {
        stm = gmtime_r(&t, &tmr);
        s++;
    } else {
        stm = localtime_r(&t, &tmr);
    }

    if (stm == NULL)
        return luaL_error(L, "date result cannot be represented in this installation");

    if (strcmp(s, "*t") == 0) {
        lua_createtable(L, 0, 9);
        lua_pushinteger(L, stm->tm_year + 1900); lua_setfield(L, -2, "year");
        lua_pushinteger(L, stm->tm_mon + 1);     lua_setfield(L, -2, "month");
        lua_pushinteger(L, stm->tm_mday);         lua_setfield(L, -2, "day");
        lua_pushinteger(L, stm->tm_hour);          lua_setfield(L, -2, "hour");
        lua_pushinteger(L, stm->tm_min);           lua_setfield(L, -2, "min");
        lua_pushinteger(L, stm->tm_sec);           lua_setfield(L, -2, "sec");
        lua_pushinteger(L, stm->tm_wday + 1);     lua_setfield(L, -2, "wday");
        lua_pushinteger(L, stm->tm_yday + 1);     lua_setfield(L, -2, "yday");
        lua_pushboolean(L, stm->tm_isdst);         lua_setfield(L, -2, "isdst");
        return 1;
    }

    char buf[256];
    size_t len = strftime(buf, sizeof(buf), s, stm);
    if (len == 0)
        return luaL_error(L, "date format too long");
    lua_pushlstring(L, buf, len);
    return 1;
}

static int os_difftime (lua_State *L) {
    time_t t2 = (time_t)luaL_checkinteger(L, 1);
    time_t t1 = (time_t)luaL_optinteger(L, 2, 0);
    lua_pushnumber(L, (lua_Number)difftime(t2, t1));
    return 1;
}

static int os_setlocale (lua_State *L) {
    const char *l = luaL_optstring(L, 1, NULL);
    int cat = luaL_optinteger(L, 2, LC_ALL);
    lua_pushstring(L, setlocale(cat, l));
    return 1;
}

static int os_execute (lua_State *L) {
    (void)L;

    if (lua_isnoneornil(L, 1)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushnil(L);
    lua_pushliteral(L, "not available");
    lua_pushinteger(L, -1);
    return 3;
}

static int os_exit (lua_State *L) {
    int status = (int)luaL_optinteger(L, 1, EXIT_SUCCESS);
    (void)status;

    exit(status);
    return 0;
}

static int os_getenv (lua_State *L) {
    (void)L;
    lua_pushnil(L);
    return 1;
}

static int os_remove (lua_State *L) {
    const char *filename = luaL_checkstring(L, 1);
    return luaL_fileresult(L, remove(filename) == 0, filename);
}

static int os_rename (lua_State *L) {
    const char *fromname = luaL_checkstring(L, 1);
    const char *toname = luaL_checkstring(L, 2);
    return luaL_fileresult(L, rename(fromname, toname) == 0, NULL);
}

static int os_tmpname (lua_State *L) {
    return luaL_error(L, "os.tmpname not available (no filesystem)");
}

static const luaL_Reg syslib[] = {
    {"clock",     os_clock},
    {"date",      os_date},
    {"difftime",  os_difftime},
    {"execute",   os_execute},
    {"exit",      os_exit},
    {"getenv",    os_getenv},
    {"remove",    os_remove},
    {"rename",    os_rename},
    {"setlocale", os_setlocale},
    {"time",      os_time},
    {"tmpname",   os_tmpname},
    {NULL, NULL}
};

LUAMOD_API int luaopen_os (lua_State *L) {
    luaL_newlib(L, syslib);
    return 1;
}