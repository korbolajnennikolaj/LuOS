#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

lua_State *luos_lua_init(void) {
    lua_State *L = luaL_newstate();
    if (!L) {
        printf("ERROR: failed to create Lua state (out of memory?)\n");
        return (lua_State*)0;
    }

    luaL_openlibs(L);

    return L;
}

void luos_lua_close(lua_State *L) {
    if (L) lua_close(L);
}

int luos_lua_dostring(lua_State *L, const char *code) {
    if (!L || !code) return -1;

    int status = luaL_dostring(L, code);
    if (status != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        if (msg) {
            printf("Lua error: %s\n", msg);
        } else {
            printf("Lua error: (unknown)\n");
        }
        lua_pop(L, 1);
    }
    return status;
}

int luos_lua_dostring_color(lua_State *L, const char *code, uint32_t color) {
    (void)color;
    return luos_lua_dostring(L, code);
}

void luos_lua_repl(lua_State *L) {
    if (!L) return;

    printf("Lua 5.5.1 on LuOS\n");
    printf("Type Lua code. 'quit' to exit.\n");

    for (;;) {
        printf("> ");

        luos_lua_dostring(L, "print('Hello from Lua on LuOS!')");
        luos_lua_dostring(L, "print('Lua version: ' .. _VERSION)");
        luos_lua_dostring(L, "print('2 + 2 = ' .. tostring(2+2))");
        luos_lua_dostring(L, "print('math.pi = ' .. tostring(math.pi))");
        luos_lua_dostring(L, "print('math.sqrt(144) = ' .. tostring(math.sqrt(144)))");
        luos_lua_dostring(L, "print(os.date('%Y-%m-%d %H:%M:%S'))");

        luos_lua_dostring(L,
            "local t = {10, 20, 30, 40, 50}\n"
            "local sum = 0\n"
            "for i, v in ipairs(t) do sum = sum + v end\n"
            "print('sum of {10,20,30,40,50} = ' .. sum)"
        );

        break;
    }
}
