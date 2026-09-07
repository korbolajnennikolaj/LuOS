#define LUA_LIB

#include "lprefix.h"

#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static int ll_loadlib (lua_State *L) {
    (void)L;
    lua_pushnil(L);
    lua_pushstring(L, "dynamic libraries not available");
    lua_pushstring(L, "absent");
    return 3;
}

static int searcher_preload (lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
    if (lua_getfield(L, -1, name) == LUA_TNIL) {
        lua_pushfstring(L, "no field package.preload['%s']", name);
        return 1;
    }
    lua_pushstring(L, ":preload:");
    return 2;
}

static const luaL_Reg pk_funcs[] = {
    {"loadlib", ll_loadlib},

    {NULL, NULL}
};

static void createSearchersTable (lua_State *L) {
    lua_createtable(L, 1, 0);
    lua_pushcfunction(L, searcher_preload);
    lua_rawseti(L, -2, 1);
}

static void createsearcherstable (lua_State *L) {
    createSearchersTable(L);

    lua_setfield(L, -2, "searchers");
}

static int ll_require (lua_State *L) {
    const char *name = luaL_checkstring(L, 1);

    lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_getfield(L, -1, name);
    if (lua_toboolean(L, -1))
        return 1;
    lua_pop(L, 1);

    lua_getfield(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
    if (lua_getfield(L, -1, name) == LUA_TNIL) {
        return luaL_error(L, "module '%s' not found\n\tno field package.preload['%s']",
                          name, name);
    }
    lua_pop(L, 1);

    lua_pushstring(L, name);
    lua_call(L, 1, 1);

    if (!lua_isnil(L, -1))
        lua_setfield(L, -3, name);

    lua_getfield(L, -2, name);
    if (lua_isnil(L, -1)) {
        lua_pushboolean(L, 1);
        lua_copy(L, -1, -2);
        lua_setfield(L, -4, name);
    }
    return 1;
}

LUAMOD_API int luaopen_package (lua_State *L) {
    luaL_newlib(L, pk_funcs);

    luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_setfield(L, -2, "loaded");

    luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
    lua_setfield(L, -2, "preload");

    createsearcherstable(L);

    lua_pushliteral(L, "/\n;\n?\n!\n-");
    lua_setfield(L, -2, "config");

    lua_pushliteral(L, "");
    lua_setfield(L, -2, "path");
    lua_pushliteral(L, "");
    lua_setfield(L, -2, "cpath");

    lua_pushcfunction(L, ll_require);
    lua_setglobal(L, "require");

    return 1;
}