#include "service_shell.h"

#include "service_root_fs.h"
#include "service_keyboard_updater.h"
#include "service_mouse_updater.h"
#include "service_usb_hotplug.h"

#include <stdlib.h>

#include "components/drivers.h"
#include "components/panic.h"
#include "components/GDT/gdt.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/tsc_driver.h"
#include "drivers/Timer/pit_driver.h"
#include "drivers/Timer/hpet_driver.h"
#include "drivers/Input/keyboard_driver.h"
#include "drivers/Input/mouse_driver.h"
#include "drivers/Video/limine_video_driver.h"
#include "components/pci.h"
#include "kernel/games.h"
#include "kernel/kernel_info.h"
#include "drivers/Storage/block_device.h"
#include "drivers/Storage/ahci.h"
#include "drivers/Storage/partition.h"
#include "drivers/Storage/usb_msc_driver.h"
#include "drivers/USB/usb_log.h"
#include "fs/fs.h"
#include "kernel/rootfs.h"
#include "kernel/math_test.h"
#include "kernel/libc_test.h"
#include "components/Memory/pmm.h"
#include "components/Memory/vmm.h"
#include "components/Memory/heap.h"
#include "components/ACPI/acpi.h"
#include "components/ACPI/power.h"
#include "components/ACPI/hpet_acpi.h"
#include "components/ACPI/mcfg.h"
#include "drivers/USB/usb_controller.h"
#include "drivers/USB/usb_core.h"
#include "drivers/USB/root_hub.h"
#include "kernel/scheduler/scheduler.h"
#include "kernel/scheduler/spinlock.h"
#include "kernel/smp/smp.h"
#include "lua/lua.h"
#include "lua/lualib.h"
#include "lua/lauxlib.h"

#include <cpuid.h>
#include <math.h>
#include <ports.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

extern lua_State *luos_lua_init(void);
extern void luos_lua_close(lua_State *L);
extern int luos_lua_dostring(lua_State *L, const char *code);
extern void luos_lua_test(lua_State *L);

#include "kernel/console.h"

#include "mp_luos.h"
#include "mp_kernel.h"

static lua_State *g_lua = (void*)0;
static int g_mpy_ready = 0;

static int str_cmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int str_starts_with(const char* str, const char* prefix) {
    while (*prefix) {
        if (*str != *prefix) return 0;
        str++; prefix++;
    }
    return 1;
}

static unsigned int parse_number(const char* str) {
    unsigned int result = 0;
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    return result;
}



static void print_u64(struct limine_video_driver* video, uint64_t val, uint32_t color) {
    if (val == 0) { video->printf("0", color); return; }
    char buf[21];
    char* p = buf + 20;
    *p = '\0';
    while (val > 0) { *--p = '0' + (val % 10); val /= 10; }
    video->printf(p, color);
}

static const char* get_pci_type_name(uint8_t class_code) {
    switch (class_code) {
        case 0x01: return "Storage";
        case 0x02: return "Network";
        case 0x03: return "Display";
        case 0x04: return "Multimedia";
        case 0x06: return "Bridge";
        case 0x07: return "Comm";
        case 0x0C: return "SerialBus";
        default: return "Unknown";
    }
}

static const char* get_usb_type_name(uint8_t prog_if) {
    switch (prog_if) {
        case 0x00: return "UHCI";
        case 0x10: return "OHCI";
        case 0x20: return "EHCI";
        case 0x30: return "XHCI";
        case 0x80: return "USB-Other";
        case 0xFE: return "USB-Device";
        default: return "Unknown";
    }
}

static void draw_header(struct limine_video_driver* video) {
    video->printf("================================================================\n", LIMINE_COLOR_LIGHT_BLUE);
    video->printf("       __         ____  _____    ____  _____ \n", LIMINE_COLOR_CYAN);
    video->printf("      / /_  __  _/ __ \\/ ___/   / __ \\/ ___/ \n", LIMINE_COLOR_CYAN);
    video->printf("      / / / / / / / / / /\\__ \\   / / / /\\__ \\  \n", LIMINE_COLOR_CYAN);
    video->printf("    / /_/ /_/ / / /_/ /___/ /  / /_/ /___/ /  \n", LIMINE_COLOR_CYAN);
    video->printf("    \\____\\__,_/  \\____//____/   \\____//____/   \n", LIMINE_COLOR_CYAN);
    video->printf("\n               Welcome to LuOS Final Kernel Test\n", LIMINE_COLOR_WHITE);
    video->printf("================================================================\n", LIMINE_COLOR_LIGHT_BLUE);
}

static void draw_system_info(struct limine_video_driver* video, struct rtc_driver* rtc) {
    struct system_time* time = rtc->get_rtc_time();
    video->printf(" [SYSTEM] Status: ", LIMINE_COLOR_LIGHT_GRAY);
    video->printf("ONLINE", LIMINE_COLOR_LIGHT_GREEN);
    video->printf(" | Time: ", LIMINE_COLOR_LIGHT_GRAY);

    char t_buf[9];
    t_buf[0] = '0' + (time->hours / 10); t_buf[1] = '0' + (time->hours % 10); t_buf[2] = ':';
    t_buf[3] = '0' + (time->minutes / 10); t_buf[4] = '0' + (time->minutes % 10); t_buf[5] = ':';
    t_buf[6] = '0' + (time->seconds / 10); t_buf[7] = '0' + (time->seconds % 10); t_buf[8] = '\0';
    video->printf(t_buf, LIMINE_COLOR_AMBER);

    char vendor[13];
    cpuid_get_vendor(vendor);
    vendor[12] = '\0';
    video->printf(" | CPU: ", LIMINE_COLOR_LIGHT_GRAY);
    video->printf(vendor, LIMINE_COLOR_LIGHT_MAGENTA);

    video->printf(" | Lua: ", LIMINE_COLOR_LIGHT_GRAY);
    video->printf(g_lua ? "OK" : "N/A", g_lua ? LIMINE_COLOR_LIGHT_GREEN : LIMINE_COLOR_LIGHT_RED);

    video->printf("\n----------------------------------------------------------------\n", LIMINE_COLOR_DARK_GRAY);
}

static struct limine_video_driver* g_bind_video = (void*)0;
static struct rtc_driver* g_bind_rtc = (void*)0;
static struct pit_driver* g_bind_pit = (void*)0;
static struct tsc_driver* g_bind_tsc = (void*)0;

static int lua_kernel_print(lua_State *L) {
    const char *text = luaL_checkstring(L, 1);
    if (g_bind_video) {
        g_bind_video->printf(text, LIMINE_COLOR_WHITE);
        g_bind_video->printf("\n", LIMINE_COLOR_WHITE);
    } else {
        printf("%s\n", text);
    }
    return 0;
}

static int lua_kernel_print_color(lua_State *L) {
    const char *text = luaL_checkstring(L, 1);
    lua_Integer r = luaL_checkinteger(L, 2);
    lua_Integer g = luaL_checkinteger(L, 3);
    lua_Integer b = luaL_checkinteger(L, 4);

    uint32_t color = 0xFF000000u
                   | ((uint32_t)(r & 0xFF) << 16)
                   | ((uint32_t)(g & 0xFF) << 8)
                   | ((uint32_t)(b & 0xFF));

    if (g_bind_video) {
        g_bind_video->printf(text, color);
        g_bind_video->printf("\n", color);
    } else {
        printf("%s\n", text);
    }
    return 0;
}

static int lua_kernel_clear(lua_State *L) {
    (void)L;
    if (g_bind_video) {
        g_bind_video->clear(LIMINE_COLOR_BLACK);
    }
    return 0;
}

static int lua_kernel_help(lua_State *L) {
    (void)L;

    if (!g_bind_video) {
        printf("kernel.* API — LuOS Kernel Lua bindings\n");
        return 0;
    }

    uint32_t c_head = 0xFF55FFFF;
    uint32_t c_name = 0xFF55FF55;
    uint32_t c_sep = 0xFFAAAAAA;
    uint32_t c_desc = 0xFFFFFFFF;
    uint32_t c_line = 0xFF5555FF;

    g_bind_video->printf("\n", 0);
    g_bind_video->printf("================================================\n", c_line);
    g_bind_video->printf("   kernel.*  —  LuOS Lua API  (" KERNEL_VERSION ")\n", c_head);
    g_bind_video->printf("================================================\n", c_line);

#define KHELP(sig, desc) \
    g_bind_video->printf("  kernel.", c_sep); \
    g_bind_video->printf(sig, c_name); \
    g_bind_video->printf(desc, c_desc);

    g_bind_video->printf("\n [Output]\n", c_head);
    KHELP("print(text)", "         print text in white\n");
    KHELP("print_color(text,r,g,b)", " print with RGB(0-255) color\n");
    KHELP("clear()", "              clear screen to black\n");
    KHELP("fastfetch()", "           show system info panel\n");
    KHELP("help()", "               show this help\n");

    g_bind_video->printf("\n [System]\n", c_head);
    KHELP("version()", "            kernel version string\n");
    KHELP("get_cpu_vendor()", "     CPU vendor string via CPUID\n");
    KHELP("has_apic()", "           true/false — APIC present\n");
    KHELP("has_sse()", "            true/false — SSE present\n");
    KHELP("has_avx()", "            true/false — AVX present\n");

    g_bind_video->printf("\n [Time]\n", c_head);
    KHELP("get_time()", "         {hours,minutes,seconds} RTC\n");
    KHELP("get_tsc_ms()", "       uptime milliseconds via TSC\n");
    KHELP("sleep_ms(n)", "         sleep n ms (TSC/PIT fallback)\n");

    g_bind_video->printf("\n [Memory]\n", c_head);
    KHELP("get_ram_mb()", "       {total_mb, free_mb, used_mb}\n");
    KHELP("get_heap_stats()", "   {total, used, free} in bytes\n");
    KHELP("kmalloc_test(size)", "   alloc+free size bytes, return addr\n");

#undef KHELP

    g_bind_video->printf("\n================================================\n", c_line);
    g_bind_video->printf(" Example: kernel.version()  kernel.get_time()\n", c_sep);
    g_bind_video->printf(" In REPL: lua>  kernel.help()\n", c_sep);
    g_bind_video->printf("================================================\n\n", c_line);

    return 0;
}

static int lua_kernel_get_time(lua_State *L) {
    if (!g_bind_rtc) {
        lua_pushnil(L);
        lua_pushstring(L, "RTC driver not available");
        return 2;
    }
    struct system_time *t = g_bind_rtc->get_rtc_time();

    lua_createtable(L, 0, 3);

    lua_pushinteger(L, (lua_Integer)t->hours);
    lua_setfield(L, -2, "hours");

    lua_pushinteger(L, (lua_Integer)t->minutes);
    lua_setfield(L, -2, "minutes");

    lua_pushinteger(L, (lua_Integer)t->seconds);
    lua_setfield(L, -2, "seconds");

    return 1;
}

static int lua_kernel_get_cpu_vendor(lua_State *L) {
    char vendor[13];
    cpuid_get_vendor(vendor);
    vendor[12] = '\0';
    lua_pushstring(L, vendor);
    return 1;
}

static int lua_kernel_get_ram_mb(lua_State *L) {
    uint64_t free_pages = pmm_free_page_count();
    uint64_t used_pages = pmm_used_pages();
    uint64_t total_pages = free_pages + used_pages;

    lua_Integer total_mb = (lua_Integer)(total_pages / 256);
    lua_Integer free_mb = (lua_Integer)(free_pages / 256);
    lua_Integer used_mb = (lua_Integer)(used_pages / 256);

    lua_createtable(L, 0, 3);

    lua_pushinteger(L, total_mb);
    lua_setfield(L, -2, "total_mb");

    lua_pushinteger(L, free_mb);
    lua_setfield(L, -2, "free_mb");

    lua_pushinteger(L, used_mb);
    lua_setfield(L, -2, "used_mb");

    return 1;
}

static int lua_kernel_sleep_ms(lua_State *L) {
    lua_Integer ms = luaL_checkinteger(L, 1);
    if (ms <= 0) return 0;

    if (g_bind_tsc) {
        g_bind_tsc->sleep_tsc_ms((unsigned int)ms);
    } else if (g_bind_pit) {
        g_bind_pit->sleep_pit_ms((unsigned int)ms);
    }
    return 0;
}

static int lua_kernel_get_heap_stats(lua_State *L) {
    uint64_t total = heap_get_total();
    uint64_t used = heap_get_used();
    uint64_t free = (total > used) ? (total - used) : 0;

    lua_createtable(L, 0, 3);

    lua_pushinteger(L, (lua_Integer)total);
    lua_setfield(L, -2, "total");

    lua_pushinteger(L, (lua_Integer)used);
    lua_setfield(L, -2, "used");

    lua_pushinteger(L, (lua_Integer)free);
    lua_setfield(L, -2, "free");

    return 1;
}

static int lua_kernel_kmalloc_test(lua_State *L) {
    lua_Integer size = luaL_checkinteger(L, 1);
    if (size <= 0 || size > 64 * 1024 * 1024) {
        lua_pushinteger(L, 0);
        return 1;
    }

    void *ptr = kmalloc((size_t)size);
    if (!ptr) {
        lua_pushinteger(L, 0);
        return 1;
    }

    kfree(ptr);
    lua_pushinteger(L, (lua_Integer)(uintptr_t)ptr);
    return 1;
}

static int lua_kernel_get_tsc_ms(lua_State *L) {
    if (!g_bind_tsc) {
        lua_pushinteger(L, 0);
        return 1;
    }
    uint64_t ms = g_bind_tsc->get_tsc_uptime_ms();
    lua_pushinteger(L, (lua_Integer)ms);
    return 1;
}

static int lua_kernel_has_apic(lua_State *L) {
    lua_pushboolean(L, cpuid_has_apic());
    return 1;
}

static int lua_kernel_has_sse(lua_State *L) {
    struct cpuid_result info = cpuid_get_processor_info();
    lua_pushboolean(L, (info.edx & CPUID_FEATURE_SSE) != 0);
    return 1;
}

static int lua_kernel_has_avx(lua_State *L) {
    struct cpuid_result info = cpuid_get_processor_info();
    lua_pushboolean(L, (info.ecx & CPUID_FEATURE_AVX) != 0);
    return 1;
}

static int lua_kernel_version(lua_State *L) {
    lua_pushstring(L, "LuOS Kernel " KERNEL_VERSION " (" LUA_VERSION " " KERNEL_COMPILATION_DATE " " KERNEL_COMPILER ")");
    return 1;
}

static int lua_kernel_input(lua_State *L) {
    const char *prompt = luaL_optstring(L, 1, "");
    char line[512];

    int n = luos_console_readline(prompt, line, sizeof(line));
    if (n < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, line, (size_t)n);
    return 1;
}

static int lua_kernel_getkey(lua_State *L) {
    int c = luos_console_getkey();
    if (c < 0) {
        lua_pushnil(L);
        return 1;
    }
    char ch = (char)c;
    lua_pushlstring(L, &ch, 1);
    return 1;
}

static int lua_kernel_has_key(lua_State *L) {
    lua_pushboolean(L, luos_console_has_key());
    return 1;
}

static void cmd_mouse_test(struct limine_video_driver *video, struct mouse_driver *mouse, struct tsc_driver *tsc)
{
    if (!mouse) {
        video->printf("\n[mouse-test] No virtual mouse driver found.\n",
                      LIMINE_COLOR_LIGHT_RED);
        return;
    }

    int active = mouse->get_active_type();
    const char *type_str = (active == USB_MOUSE) ? "USB"
                         : (active == PS2_MOUSE) ? "PS/2"
                         : "None";

    video->printf("\n=== Mouse Driver Test ===\n", LIMINE_COLOR_CYAN);
    video->printf(" Active backend: ", LIMINE_COLOR_LIGHT_GRAY);
    video->printf(type_str,
                  (active == -1) ? LIMINE_COLOR_LIGHT_RED : LIMINE_COLOR_LIGHT_GREEN);
    video->printf("\n", 0);

    if (active == -1) {
        video->printf(" No mouse hardware detected.\n", LIMINE_COLOR_LIGHT_RED);
        video->printf("=========================\n\n", LIMINE_COLOR_CYAN);
        return;
    }

    mouse_sample_t sample;
    mouse_updater_take(&sample);

    video->printf(" Initial state:\n", LIMINE_COLOR_WHITE);
    video->printf("   btn_left=", LIMINE_COLOR_LIGHT_GRAY);
    video->printf(sample.btn_left ? "1" : "0", LIMINE_COLOR_AMBER);
    video->printf("  btn_right=", LIMINE_COLOR_LIGHT_GRAY);
    video->printf(sample.btn_right ? "1" : "0", LIMINE_COLOR_AMBER);
    video->printf("  btn_middle=", LIMINE_COLOR_LIGHT_GRAY);
    video->printf(sample.btn_middle ? "1" : "0", LIMINE_COLOR_AMBER);
    video->printf("\n", 0);

    video->printf("\n Move the mouse or click buttons.\n", LIMINE_COLOR_LIGHT_GRAY);
    video->printf(" Polling for 5 seconds (10 samples x 500 ms)...\n\n",
                  LIMINE_COLOR_DARK_GRAY);

    #define SAMPLES 10
    #define SAMPLE_MS 500

    int motion_detected = 0;
    int button_detected = 0;
    int32_t total_dx = 0, total_dy = 0;

    for (int s = 0; s < SAMPLES; s++) {

        mouse_updater_take(&sample);

        int32_t dx = sample.dx;
        int32_t dy = sample.dy;
        total_dx += dx;
        total_dy += dy;

        if (dx || dy) motion_detected = 1;
        if (sample.btn_left || sample.btn_right || sample.btn_middle) button_detected = 1;

        char snum[3] = { '0' + (s / 10), '0' + (s % 10), '\0' };
        video->printf(" [", LIMINE_COLOR_DARK_GRAY);
        video->printf(snum, LIMINE_COLOR_LIGHT_GRAY);
        video->printf("] ", LIMINE_COLOR_DARK_GRAY);

        video->printf("dx=", LIMINE_COLOR_LIGHT_GRAY);
        if (dx < 0) { video->printf("-", LIMINE_COLOR_LIGHT_RED); dx = -dx; }
        else { }
        print_u64(video, (uint64_t)dx, LIMINE_COLOR_AMBER);

        video->printf("  dy=", LIMINE_COLOR_LIGHT_GRAY);
        if (dy < 0) { video->printf("-", LIMINE_COLOR_LIGHT_RED); dy = -dy; }
        print_u64(video, (uint64_t)dy, LIMINE_COLOR_AMBER);

        video->printf("  wheel=", LIMINE_COLOR_LIGHT_GRAY);
        print_u64(video, (uint64_t)(sample.wheel < 0 ? -sample.wheel : sample.wheel),
                  LIMINE_COLOR_LIGHT_CYAN);

        video->printf("  L=", LIMINE_COLOR_LIGHT_GRAY);
        video->printf(sample.btn_left ? "1" : "0",
                      sample.btn_left ? LIMINE_COLOR_LIGHT_GREEN : LIMINE_COLOR_DARK_GRAY);
        video->printf(" R=", LIMINE_COLOR_LIGHT_GRAY);
        video->printf(sample.btn_right ? "1" : "0",
                      sample.btn_right ? LIMINE_COLOR_LIGHT_GREEN : LIMINE_COLOR_DARK_GRAY);
        video->printf(" M=", LIMINE_COLOR_LIGHT_GRAY);
        video->printf(sample.btn_middle ? "1" : "0",
                      sample.btn_middle ? LIMINE_COLOR_LIGHT_GREEN : LIMINE_COLOR_DARK_GRAY);
        video->printf("\n", 0);

        if (tsc) tsc->sleep_tsc_ms(SAMPLE_MS);
    }

    #undef SAMPLES
    #undef SAMPLE_MS

    video->printf("\n--- Summary ---\n", LIMINE_COLOR_LIGHT_BLUE);

    video->printf(" Motion:  ", LIMINE_COLOR_LIGHT_GRAY);
    video->printf(motion_detected ? "[DETECTED]" : "[NONE]",
                  motion_detected ? LIMINE_COLOR_LIGHT_GREEN : LIMINE_COLOR_AMBER);
    video->printf("\n", 0);

    video->printf(" Buttons: ", LIMINE_COLOR_LIGHT_GRAY);
    video->printf(button_detected ? "[DETECTED]" : "[NONE]",
                  button_detected ? LIMINE_COLOR_LIGHT_GREEN : LIMINE_COLOR_AMBER);
    video->printf("\n", 0);

    video->printf(" Total delta  X=", LIMINE_COLOR_LIGHT_GRAY);
    if (total_dx < 0) { video->printf("-", LIMINE_COLOR_LIGHT_RED); total_dx = -total_dx; }
    print_u64(video, (uint64_t)total_dx, LIMINE_COLOR_AMBER);
    video->printf("  Y=", LIMINE_COLOR_LIGHT_GRAY);
    if (total_dy < 0) { video->printf("-", LIMINE_COLOR_LIGHT_RED); total_dy = -total_dy; }
    print_u64(video, (uint64_t)total_dy, LIMINE_COLOR_AMBER);
    video->printf("\n", 0);

    video->printf(" Result:  ", LIMINE_COLOR_LIGHT_GRAY);
    if (motion_detected || button_detected)
        video->printf("[PASS] Mouse is working correctly.\n", LIMINE_COLOR_LIGHT_GREEN);
    else
        video->printf("[WARN] No input detected. Is a mouse connected?\n", LIMINE_COLOR_AMBER);

    video->printf("=========================\n\n", LIMINE_COLOR_CYAN);
}

static void cmd_fastfetch(struct limine_video_driver* video);

static int lua_kernel_fastfetch(lua_State *L) {
    (void)L;
    cmd_fastfetch(g_bind_video);
    return 0;
}

static const luaL_Reg kernel_lib[] = {
    { "print", lua_kernel_print },
    { "print_color", lua_kernel_print_color },
    { "clear", lua_kernel_clear },
    { "help", lua_kernel_help },
    { "get_time", lua_kernel_get_time },
    { "get_cpu_vendor", lua_kernel_get_cpu_vendor },
    { "get_ram_mb", lua_kernel_get_ram_mb },
    { "sleep_ms", lua_kernel_sleep_ms },
    { "get_heap_stats", lua_kernel_get_heap_stats },
    { "kmalloc_test", lua_kernel_kmalloc_test },
    { "get_tsc_ms", lua_kernel_get_tsc_ms },
    { "has_apic", lua_kernel_has_apic },
    { "has_sse", lua_kernel_has_sse },
    { "has_avx", lua_kernel_has_avx },
    { "version", lua_kernel_version },
    { "fastfetch", lua_kernel_fastfetch },
    { "input", lua_kernel_input },
    { "getkey", lua_kernel_getkey },
    { "has_key", lua_kernel_has_key },
    { NULL, NULL }
};

static void luos_register_kernel_lib(lua_State *L) {
    lua_newtable(L);

    const luaL_Reg *p = kernel_lib;
    for (; p->name != (void*)0; p++) {
        lua_pushcfunction(L, p->func);
        lua_setfield(L, -2, p->name);
    }

    lua_setglobal(L, "kernel");
}

static void cmd_lua_kernel_bindings_test(struct limine_video_driver* video) {
    if (!g_lua) {
        video->printf("Lua not initialized!\n", LIMINE_COLOR_LIGHT_RED);
        return;
    }

    printf_color(LIMINE_COLOR_LIGHT_BLUE, "============================================\n");
    printf_color(LIMINE_COLOR_CYAN, "   kernel.* Bindings Test Suite\n");
    printf_color(LIMINE_COLOR_LIGHT_BLUE, "============================================\n\n");

    printf_color(LIMINE_COLOR_AMBER, "[1] kernel.version():\n");
    luos_lua_dostring(g_lua, "print('  ' .. kernel.version())");

    printf_color(LIMINE_COLOR_AMBER, "[2] kernel.get_cpu_vendor():\n");
    luos_lua_dostring(g_lua, "print('  CPU: ' .. kernel.get_cpu_vendor())");

    printf_color(LIMINE_COLOR_AMBER, "[3] kernel.get_time():\n");
    luos_lua_dostring(g_lua,
        "local t = kernel.get_time()\n"
        "print(string.format('  Time: %02d:%02d:%02d', t.hours, t.minutes, t.seconds))"
    );

    printf_color(LIMINE_COLOR_AMBER, "[4] kernel.get_ram_mb():\n");
    luos_lua_dostring(g_lua,
        "local r = kernel.get_ram_mb()\n"
        "print('  Total: ' .. r.total_mb .. ' MB')\n"
        "print('  Free:  ' .. r.free_mb  .. ' MB')\n"
        "print('  Used:  ' .. r.used_mb  .. ' MB')"
    );

    printf_color(LIMINE_COLOR_AMBER, "[5] kernel.get_heap_stats():\n");
    luos_lua_dostring(g_lua,
        "local h = kernel.get_heap_stats()\n"
        "print('  Heap total: ' .. h.total .. ' bytes')\n"
        "print('  Heap used:  ' .. h.used  .. ' bytes')\n"
        "print('  Heap free:  ' .. h.free  .. ' bytes')"
    );

    printf_color(LIMINE_COLOR_AMBER, "[6] kernel.get_tsc_ms():\n");
    luos_lua_dostring(g_lua,
        "local ms = kernel.get_tsc_ms()\n"
        "print('  Uptime: ' .. ms .. ' ms')"
    );

    printf_color(LIMINE_COLOR_AMBER, "[7] CPU feature flags:\n");
    luos_lua_dostring(g_lua,
        "print('  APIC: ' .. tostring(kernel.has_apic()))\n"
        "print('  SSE:  ' .. tostring(kernel.has_sse()))\n"
        "print('  AVX:  ' .. tostring(kernel.has_avx()))"
    );

    printf_color(LIMINE_COLOR_AMBER, "[8] kernel.kmalloc_test(256):\n");
    luos_lua_dostring(g_lua,
        "local addr = kernel.kmalloc_test(256)\n"
        "if addr ~= 0 then\n"
        "  print('  [PASS] allocated at 0x' .. string.format('%X', addr))\n"
        "else\n"
        "  print('  [FAIL] kmalloc returned 0')\n"
        "end"
    );

    printf_color(LIMINE_COLOR_AMBER, "[9] kernel.print / kernel.print_color:\n");
    luos_lua_dostring(g_lua, "kernel.print('  Hello from kernel.print!')");
    luos_lua_dostring(g_lua, "kernel.print_color('  Colored text (green)!', 0, 255, 0)");
    luos_lua_dostring(g_lua, "kernel.print_color('  Colored text (cyan)!',  0, 255, 255)");

    printf_color(LIMINE_COLOR_AMBER, "[10] kernel.sleep_ms(100) [TSC]:\n");
    luos_lua_dostring(g_lua,
        "local t0 = kernel.get_tsc_ms()\n"
        "kernel.sleep_ms(100)\n"
        "local t1 = kernel.get_tsc_ms()\n"
        "local delta = t1 - t0\n"
        "print('  Slept ~' .. delta .. ' ms (expected ~100)')\n"
        "if delta >= 80 and delta <= 200 then\n"
        "  print('  [PASS]')\n"
        "else\n"
        "  print('  [WARN] delta out of expected range')\n"
        "end"
    );

    printf_color(LIMINE_COLOR_AMBER, "[11] kernel.fastfetch():\n");
    luos_lua_dostring(g_lua, "kernel.fastfetch()");

    printf_color(LIMINE_COLOR_AMBER, "[12] kernel.help():\n");
    luos_lua_dostring(g_lua, "kernel.help()");

    printf_color(LIMINE_COLOR_AMBER, "[13] kernel.clear():\n");
    luos_lua_dostring(g_lua,
        "kernel.sleep_ms(400)\n"
        "kernel.clear()\n"
        "kernel.print_color('  Screen cleared by kernel.clear()!', 0, 255, 128)\n"
        "kernel.print('  Restoring in 1 second...')\n"
        "kernel.sleep_ms(1000)"
    );
    video->clear(LIMINE_COLOR_BLACK);
    draw_header(video);

    printf_color(LIMINE_COLOR_AMBER, "[14] Complex Lua script using kernel API:\n");
    luos_lua_dostring(g_lua,
        "local function fib(n)\n"
        "  if n < 2 then return n end\n"
        "  return fib(n-1) + fib(n-2)\n"
        "end\n"
        "local t0 = kernel.get_tsc_ms()\n"
        "local result = fib(25)\n"
        "local t1 = kernel.get_tsc_ms()\n"
        "local ram  = kernel.get_ram_mb()\n"
        "local time = kernel.get_time()\n"
        "kernel.print_color('  === Kernel API Report ===', 0, 200, 255)\n"
        "print('  fib(25) = ' .. result)\n"
        "print('  Computed in: ' .. (t1 - t0) .. ' ms')\n"
        "print('  Free RAM:    ' .. ram.free_mb .. ' MB')\n"
        "print(string.format('  Kernel time: %02d:%02d:%02d',\n"
        "      time.hours, time.minutes, time.seconds))\n"
        "kernel.print_color('  CPU: ' .. kernel.get_cpu_vendor(), 255, 200, 0)"
    );

    printf_color(LIMINE_COLOR_LIGHT_BLUE, "\n============================================\n");
    printf_color(LIMINE_COLOR_LIGHT_GREEN, "  kernel.* binding tests complete!\n");
    printf_color(LIMINE_COLOR_LIGHT_BLUE, "============================================\n\n");
}

static void cmd_lua_test(struct limine_video_driver* video) {
    if (!g_lua) {
        video->printf("Lua not initialized!\n", LIMINE_COLOR_LIGHT_RED);
        return;
    }

    video->printf("\n", 0);
    printf_color(LIMINE_COLOR_LIGHT_BLUE, "============================================\n");
    printf_color(LIMINE_COLOR_CYAN, "     " LUA_VERSION " on LuOS — Test Suite\n");
    printf_color(LIMINE_COLOR_LIGHT_BLUE, "============================================\n\n");

    printf_color(LIMINE_COLOR_AMBER, "[1] Basic types and print:\n");
    luos_lua_dostring(g_lua, "print('  Hello from Lua on LuOS!')");
    luos_lua_dostring(g_lua, "print('  Version: ' .. _VERSION)");
    luos_lua_dostring(g_lua, "print('  nil = ' .. tostring(nil))");
    luos_lua_dostring(g_lua, "print('  true = ' .. tostring(true))");
    luos_lua_dostring(g_lua, "print('  42 = ' .. tostring(42))");
    luos_lua_dostring(g_lua, "print('  3.14 = ' .. tostring(3.14))");
    printf("\n");

    printf_color(LIMINE_COLOR_AMBER, "[2] Arithmetic:\n");
    luos_lua_dostring(g_lua, "print('  2 + 2 = ' .. (2 + 2))");
    luos_lua_dostring(g_lua, "print('  7 * 8 = ' .. (7 * 8))");
    luos_lua_dostring(g_lua, "print('  100 / 3 = ' .. (100 / 3))");
    luos_lua_dostring(g_lua, "print('  100 // 3 = ' .. (100 // 3))");
    luos_lua_dostring(g_lua, "print('  2^10 = ' .. (2^10))");
    luos_lua_dostring(g_lua, "print('  17 % 5 = ' .. (17 % 5))");
    printf("\n");

    printf_color(LIMINE_COLOR_AMBER, "[3] Strings:\n");
    luos_lua_dostring(g_lua, "print('  len(\"LuOS\") = ' .. #'LuOS')");
    luos_lua_dostring(g_lua, "print('  upper = ' .. string.upper('hello'))");
    luos_lua_dostring(g_lua, "print('  sub = ' .. string.sub('LuOS Kernel', 1, 4))");
    luos_lua_dostring(g_lua, "print('  rep = ' .. string.rep('*', 10))");
    luos_lua_dostring(g_lua, "print('  format = ' .. string.format('%d + %d = %d', 2, 3, 5))");
    luos_lua_dostring(g_lua, "print('  find = ' .. tostring(string.find('Hello World', 'World')))");
    printf("\n");

    printf_color(LIMINE_COLOR_AMBER, "[4] Tables:\n");
    luos_lua_dostring(g_lua,
        "local t = {10, 20, 30, 40, 50}\n"
        "local sum = 0\n"
        "for i, v in ipairs(t) do sum = sum + v end\n"
        "print('  sum{10..50} = ' .. sum)"
    );
    luos_lua_dostring(g_lua,
        "local t = {name='LuOS', ver='0.6', lang='Lua'}\n"
        "print('  name=' .. t.name .. ' ver=' .. t.ver .. ' lang=' .. t.lang)"
    );
    luos_lua_dostring(g_lua,
        "local t = {}\n"
        "for i = 1, 100 do t[i] = i * i end\n"
        "print('  t[100] = ' .. t[100] .. ' (should be 10000)')"
    );
    printf("\n");

    printf_color(LIMINE_COLOR_AMBER, "[5] Functions & Recursion:\n");
    luos_lua_dostring(g_lua,
        "local function fib(n)\n"
        "  if n < 2 then return n end\n"
        "  return fib(n-1) + fib(n-2)\n"
        "end\n"
        "print('  fib(20) = ' .. fib(20))"
    );
    luos_lua_dostring(g_lua,
        "local function fact(n)\n"
        "  if n <= 1 then return 1 end\n"
        "  return n * fact(n-1)\n"
        "end\n"
        "print('  fact(10) = ' .. fact(10))"
    );
    printf("\n");

    printf_color(LIMINE_COLOR_AMBER, "[6] Math library:\n");
    luos_lua_dostring(g_lua, "print('  pi = ' .. math.pi)");
    luos_lua_dostring(g_lua, "print('  sqrt(144) = ' .. math.sqrt(144))");
    luos_lua_dostring(g_lua, "print('  sin(pi/2) = ' .. math.sin(math.pi/2))");
    luos_lua_dostring(g_lua, "print('  cos(0) = ' .. math.cos(0))");
    luos_lua_dostring(g_lua, "print('  log(e) = ' .. math.log(math.exp(1)))");
    luos_lua_dostring(g_lua, "print('  floor(3.7) = ' .. math.floor(3.7))");
    luos_lua_dostring(g_lua, "print('  ceil(3.2) = ' .. math.ceil(3.2))");
    luos_lua_dostring(g_lua, "print('  abs(-42) = ' .. math.abs(-42))");
    luos_lua_dostring(g_lua, "print('  max(1,5,3) = ' .. math.max(1,5,3))");
    luos_lua_dostring(g_lua, "print('  random(1,100) = ' .. math.random(1,100))");
    printf("\n");

    printf_color(LIMINE_COLOR_AMBER, "[7] Coroutines:\n");
    luos_lua_dostring(g_lua,
        "local co = coroutine.create(function()\n"
        "  for i = 1, 3 do\n"
        "    coroutine.yield(i * 10)\n"
        "  end\n"
        "  return 'done'\n"
        "end)\n"
        "local results = {}\n"
        "for i = 1, 4 do\n"
        "  local ok, val = coroutine.resume(co)\n"
        "  results[i] = tostring(val)\n"
        "end\n"
        "print('  yields: ' .. table.concat(results, ', '))"
    );
    printf("\n");

    printf_color(LIMINE_COLOR_AMBER, "[8] Error handling (pcall):\n");
    luos_lua_dostring(g_lua,
        "local ok, err = pcall(function() error('test error') end)\n"
        "print('  pcall caught: ' .. tostring(not ok) .. ' msg: ' .. tostring(err))"
    );
    luos_lua_dostring(g_lua,
        "local ok, val = pcall(function() return 42 end)\n"
        "print('  pcall success: ' .. tostring(ok) .. ' val: ' .. tostring(val))"
    );
    printf("\n");

    printf_color(LIMINE_COLOR_AMBER, "[9] Metatables:\n");
    luos_lua_dostring(g_lua,
        "local Vec = {}\n"
        "Vec.__index = Vec\n"
        "function Vec.new(x, y) return setmetatable({x=x, y=y}, Vec) end\n"
        "function Vec:__tostring() return '(' .. self.x .. ',' .. self.y .. ')' end\n"
        "function Vec.__add(a, b) return Vec.new(a.x+b.x, a.y+b.y) end\n"
        "local v = Vec.new(3, 4) + Vec.new(1, 2)\n"
        "print('  Vec(3,4) + Vec(1,2) = ' .. tostring(v))"
    );
    printf("\n");

    printf_color(LIMINE_COLOR_AMBER, "[10] Closures & Iterators:\n");
    luos_lua_dostring(g_lua,
        "local function counter(start)\n"
        "  local n = start\n"
        "  return function()\n"
        "    n = n + 1\n"
        "    return n\n"
        "  end\n"
        "end\n"
        "local c = counter(100)\n"
        "print('  counter: ' .. c() .. ', ' .. c() .. ', ' .. c())"
    );
    printf("\n");

    printf_color(LIMINE_COLOR_AMBER, "[11] OS library:\n");
    luos_lua_dostring(g_lua, "print('  os.clock() = ' .. os.clock())");
    luos_lua_dostring(g_lua, "print('  os.time() = ' .. os.time())");
    luos_lua_dostring(g_lua,
        "local ok, result = pcall(os.date, '%Y-%m-%d %H:%M:%S')\n"
        "if ok then print('  os.date() = ' .. result)\n"
        "else print('  os.date() = (error: ' .. result .. ')') end"
    );
    printf("\n");

    printf_color(LIMINE_COLOR_AMBER, "[12] Performance benchmark:\n");
    luos_lua_dostring(g_lua,
        "local t0 = os.clock()\n"
        "local sum = 0\n"
        "for i = 1, 1000000 do sum = sum + i end\n"
        "local t1 = os.clock()\n"
        "print('  sum(1..1M) = ' .. sum)\n"
        "print('  time: ' .. string.format('%.3f', t1 - t0) .. ' sec')"
    );
    printf("\n");

    printf_color(LIMINE_COLOR_LIGHT_BLUE, "============================================\n");
    printf_color(LIMINE_COLOR_LIGHT_GREEN, "  All Lua tests completed!\n");
    printf_color(LIMINE_COLOR_LIGHT_BLUE, "============================================\n\n");
}

static void cmd_lua_repl(struct limine_video_driver* video, struct keyboard_driver* kbd)
{
    if (!g_lua) {
        video->printf("Lua not initialized!\n", LIMINE_COLOR_LIGHT_RED);
        return;
    }

    printf_color(LIMINE_COLOR_CYAN, "\n=== " LUA_VERSION " Interactive Mode ===\n");
    printf_color(LIMINE_COLOR_LIGHT_GRAY, "Type Lua code. 'exit' to return to shell.\n");
    printf_color(LIMINE_COLOR_DARK_GRAY, "Tip: kernel.help() — list all kernel.* methods\n\n");

    char line[512];
    while (1) {
        kbd->input("lua> ", line, 512, LIMINE_COLOR_LIGHT_GREEN, video->printf);

        if (str_cmp(line, "exit") == 0 || str_cmp(line, "quit") == 0) {
            printf_color(LIMINE_COLOR_LIGHT_GRAY, "Leaving Lua REPL.\n\n");
            break;
        }

        if (line[0] == '\0') continue;

        char try_expr[600];
        snprintf(try_expr, sizeof(try_expr), "return %s", line);

        int status = luaL_loadstring(g_lua, try_expr);
        if (status == LUA_OK) {
            status = lua_pcall(g_lua, 0, LUA_MULTRET, 0);
            if (status == LUA_OK) {
                int nresults = lua_gettop(g_lua);
                if (nresults > 0) {
                    for (int i = 1; i <= nresults; i++) {
                        if (i > 1) printf("\t");
                        const char *s = luaL_tolstring(g_lua, i, (void*)0);
                        if (s) printf("%s", s);
                        lua_pop(g_lua, 1);
                    }
                    printf("\n");
                }
                lua_settop(g_lua, 0);
            } else {
                const char *msg = lua_tostring(g_lua, -1);
                if (msg) printf_color(LIMINE_COLOR_LIGHT_RED, "Error: %s\n", msg);
                lua_pop(g_lua, 1);
            }
        } else {
            lua_pop(g_lua, 1);
            luos_lua_dostring(g_lua, line);
        }
    }
}

static void cmd_python_repl(struct limine_video_driver* video, struct keyboard_driver* kbd)
{
    if (!g_mpy_ready) {
        video->printf("MicroPython not initialized!\n", LIMINE_COLOR_LIGHT_RED);
        return;
    }

    printf_color(LIMINE_COLOR_CYAN, "\n=== MicroPython Interactive Mode ===\n");
    printf_color(LIMINE_COLOR_LIGHT_GRAY, "Type Python code. 'exit' to return to shell.\n\n");

    char line[512];
    while (1) {
        kbd->input("py> ", line, 512, LIMINE_COLOR_LIGHT_GREEN, video->printf);

        if (str_cmp(line, "exit") == 0 || str_cmp(line, "quit") == 0) {
            printf_color(LIMINE_COLOR_LIGHT_GRAY, "Leaving Python REPL.\n\n");
            break;
        }

        if (line[0] == '\0') continue;

        mp_luos_exec_str(line);
    }
}

static void cmd_lua_io_test(struct limine_video_driver* video) {

    luos_console_flush_input();

    if (!g_lua) {
        video->printf("Lua not initialized!\n", LIMINE_COLOR_LIGHT_RED);
        return;
    }

    printf_color(LIMINE_COLOR_LIGHT_BLUE, "============================================\n");
    printf_color(LIMINE_COLOR_CYAN,       "   Lua io.* keyboard test\n");
    printf_color(LIMINE_COLOR_LIGHT_BLUE, "============================================\n");
    printf_color(LIMINE_COLOR_DARK_GRAY,  "Ctrl+D ends input, Ctrl+C aborts a read.\n\n");

    printf_color(LIMINE_COLOR_AMBER, "[1] io.read()  - one line\n");
    luos_lua_dostring(g_lua,
        "io.write('  your name: ')\n"
        "local name = io.read()\n"
        "if name == nil then print('  (end of input)')\n"
        "else print('  hello, ' .. name .. '! (' .. #name .. ' chars)') end"
    );

    printf_color(LIMINE_COLOR_AMBER, "\n[2] io.read('n') - a number\n");
    luos_lua_dostring(g_lua,
        "io.write('  a number: ')\n"
        "local n = io.read('n')\n"
        "if n == nil then print('  (not a number)')\n"
        "else print('  n * 2 = ' .. (n * 2)) end"
    );

    printf_color(LIMINE_COLOR_AMBER, "\n[3] io.read(5) - exactly 5 characters\n");
    luos_lua_dostring(g_lua,
        "io.write('  type 5+ chars: ')\n"
        "local s = io.read(5)\n"
        "print('  got: [' .. tostring(s) .. ']')"
    );

    printf_color(LIMINE_COLOR_AMBER, "\n[4] io.lines() - loop until Ctrl+D\n");
    luos_lua_dostring(g_lua,
        "print('  type a few lines, then Ctrl+D')\n"
        "local count = 0\n"
        "for line in io.lines() do\n"
        "  count = count + 1\n"
        "  print('    line ' .. count .. ': ' .. line)\n"
        "end\n"
        "print('  total lines: ' .. count)"
    );

    printf_color(LIMINE_COLOR_LIGHT_GREEN, "\nio test done.\n\n");
}

static void cmd_lua_file(struct limine_video_driver* video, const char *path) {

    luos_console_flush_input();

    if (!g_lua) {
        video->printf("Lua not initialized!\n", LIMINE_COLOR_LIGHT_RED);
        return;
    }
    if (!path || path[0] == '\0') {
        printf_color(LIMINE_COLOR_LIGHT_RED, "Usage: lua-file <path.lua>\n");
        return;
    }
    if (!rootfs_is_mounted()) {
        printf_color(LIMINE_COLOR_LIGHT_RED, "Nothing is mounted. Use 'mount <disk>' first.\n");
        return;
    }

    int status = luaL_loadfile(g_lua, path);
    if (status != LUA_OK) {
        const char *msg = lua_tostring(g_lua, -1);
        printf_color(LIMINE_COLOR_LIGHT_RED, "lua: %s\n", msg ? msg : "load failed");
        lua_pop(g_lua, 1);
        return;
    }

    status = lua_pcall(g_lua, 0, 0, 0);
    if (status != LUA_OK) {
        const char *msg = lua_tostring(g_lua, -1);
        printf_color(LIMINE_COLOR_LIGHT_RED, "lua: %s\n", msg ? msg : "runtime error");
        lua_pop(g_lua, 1);
    }
    lua_settop(g_lua, 0);
}

static void cmd_python_file(struct limine_video_driver* video, const char *path) {

    luos_console_flush_input();

    if (!g_mpy_ready) {
        video->printf("MicroPython not initialized!\n", LIMINE_COLOR_LIGHT_RED);
        return;
    }
    if (!path || path[0] == '\0') {
        printf_color(LIMINE_COLOR_LIGHT_RED, "Usage: python-file <path.py>\n");
        return;
    }
    if (!rootfs_is_mounted()) {
        printf_color(LIMINE_COLOR_LIGHT_RED, "Nothing is mounted. Use 'mount <disk>' first.\n");
        return;
    }

    char norm[FS_MAX_PATH];
    char rel[FS_MAX_PATH];
    rootfs_normalize_path(path, norm, sizeof(norm));

    fs_t *pfs = rootfs_resolve(norm, rel, sizeof(rel));
    if (!pfs) {
        printf_color(LIMINE_COLOR_LIGHT_RED, "python: no filesystem mounted for '%s'\n", norm);
        return;
    }

    fs_file_t f;
    int r = fs_open(pfs, rel, false, false, &f);
    if (r != FS_OK) {
        printf_color(LIMINE_COLOR_LIGHT_RED, "python: cannot open '%s' (code %d)\n", norm, r);
        return;
    }

    uint64_t size = f.size;
    if (size > (8u * 1024u * 1024u)) {
        fs_close(&f);
        printf_color(LIMINE_COLOR_LIGHT_RED, "python: '%s' is too large to load\n", norm);
        return;
    }

    char *src = (char *)kmalloc((size_t)size + 1);
    if (!src) {
        fs_close(&f);
        printf_color(LIMINE_COLOR_LIGHT_RED, "python: out of memory loading '%s'\n", norm);
        return;
    }

    uint64_t got = 0;
    while (got < size) {
        int64_t n = fs_read(&f, src + got, size - got);
        if (n <= 0) break;
        got += (uint64_t)n;
    }
    fs_close(&f);
    src[got] = '\0';

    mp_luos_exec_str(src);
    kfree(src);
}

static void cmd_run_script(struct limine_video_driver* video, const char *path) {
    if (!path || path[0] == '\0') {
        printf_color(LIMINE_COLOR_LIGHT_RED, "Usage: run <path.lua|path.py>\n");
        return;
    }

    size_t len = strlen(path);
    if (len >= 4 && str_cmp(path + len - 4, ".lua") == 0) {
        cmd_lua_file(video, path);
    } else if (len >= 3 && str_cmp(path + len - 3, ".py") == 0) {
        cmd_python_file(video, path);
    } else {
        printf_color(LIMINE_COLOR_LIGHT_RED,
                     "run: don't know how to run '%s' (expected .lua or .py)\n", path);
    }
}

static void cmd_python_test(struct limine_video_driver* video) {
    if (!g_mpy_ready) {
        video->printf("MicroPython not initialized!\n", LIMINE_COLOR_LIGHT_RED);
        return;
    }

    video->printf("\n", 0);
    printf_color(LIMINE_COLOR_LIGHT_BLUE, "============================================\n");
    printf_color(LIMINE_COLOR_CYAN,       "     MicroPython on LuOS - Test Suite\n");
    printf_color(LIMINE_COLOR_LIGHT_BLUE, "============================================\n\n");

    printf_color(LIMINE_COLOR_AMBER, "[1] Basic types and print:\n");
    mp_luos_exec_str("print('  Hello from Python on LuOS!')\n");
    mp_luos_exec_str("print('  None  =', None)\n");
    mp_luos_exec_str("print('  True  =', True)\n");
    mp_luos_exec_str("print('  42    =', 42)\n");
    mp_luos_exec_str("print('  3.14  =', 3.14)\n");
    printf("\n");

    printf_color(LIMINE_COLOR_AMBER, "[2] Arithmetic:\n");
    mp_luos_exec_str("print('  2 + 2 =', 2 + 2)\n");
    mp_luos_exec_str("print('  7 * 8 =', 7 * 8)\n");
    mp_luos_exec_str("print('  100 / 3 =', 100 / 3)\n");
    mp_luos_exec_str("print('  100 // 3 =', 100 // 3)\n");
    mp_luos_exec_str("print('  2 ** 10 =', 2 ** 10)\n");
    mp_luos_exec_str("print('  17 % 5 =', 17 % 5)\n");
    printf("\n");

    printf_color(LIMINE_COLOR_AMBER, "[3] Strings:\n");
    mp_luos_exec_str("print('  len(\"LuOS\") =', len('LuOS'))\n");
    mp_luos_exec_str("print('  upper =', 'hello'.upper())\n");
    mp_luos_exec_str("print('  slice =', 'LuOS Kernel'[0:4])\n");
    mp_luos_exec_str("print('  rep =', '*' * 10)\n");
    mp_luos_exec_str("print('  format = {} + {} = {}'.format(2, 3, 5))\n");
    mp_luos_exec_str("print('  find =', 'Hello World'.find('World'))\n");
    printf("\n");

    printf_color(LIMINE_COLOR_AMBER, "[4] Lists / dicts / comprehensions:\n");
    mp_luos_exec_str("t = [10, 20, 30, 40, 50]\nprint('  sum{10..50} =', sum(t))\n");
    mp_luos_exec_str("d = {'name': 'LuOS', 'ver': '0.7', 'lang': 'Python'}\nprint('  name =', d['name'], ' lang =', d['lang'])\n");
    mp_luos_exec_str("print('  squares =', [x * x for x in range(6)])\n");
    printf("\n");

    printf_color(LIMINE_COLOR_AMBER, "[5] Functions / closures:\n");
    mp_luos_exec_str("def make_counter():\n    n = [0]\n    def inc():\n        n[0] += 1\n        return n[0]\n    return inc\nc = make_counter()\nprint('  counter:', c(), c(), c())\n");
    printf("\n");

    printf_color(LIMINE_COLOR_AMBER, "[6] Exceptions:\n");
    mp_luos_exec_str("try:\n    1 / 0\nexcept ZeroDivisionError as e:\n    print('  caught:', e)\n");
    printf("\n");

    printf_color(LIMINE_COLOR_AMBER, "[7] random module:\n");
    mp_luos_exec_str("import random\nrandom.seed(1234)\nprint('  randint(1,100) =', random.randint(1, 100))\nprint('  random()       =', random.random())\nprint('  choice([..])   =', random.choice([1, 2, 3, 4, 5]))\n");
    printf("\n");

    printf_color(LIMINE_COLOR_AMBER, "[8] time module:\n");
    mp_luos_exec_str("import time\nt0 = time.ticks_ms()\ntime.sleep_ms(50)\nt1 = time.ticks_ms()\nprint('  slept ~', time.ticks_diff(t1, t0), 'ms (expected ~50)')\n");
    printf("\n");

    printf_color(LIMINE_COLOR_AMBER, "[9] os.uname() / fs stub:\n");
    mp_luos_exec_str("import os\nu = os.uname()\nprint('  sysname =', u[0], ' release =', u[2])\ntry:\n    os.listdir('/')\nexcept OSError as e:\n    print('  os.listdir() correctly raised OSError:', e)\n");
    printf("\n");

    printf_color(LIMINE_COLOR_LIGHT_BLUE, "============================================\n");
    printf_color(LIMINE_COLOR_LIGHT_GREEN, "     Test suite complete.\n");
    printf_color(LIMINE_COLOR_LIGHT_BLUE, "============================================\n\n");
}

static void cmd_python_kernel_bindings_test(struct limine_video_driver* video) {
    if (!g_mpy_ready) {
        video->printf("MicroPython not initialized!\n", LIMINE_COLOR_LIGHT_RED);
        return;
    }

    printf_color(LIMINE_COLOR_LIGHT_BLUE, "============================================\n");
    printf_color(LIMINE_COLOR_CYAN,       "   kernel.* Bindings Test Suite (Python)\n");
    printf_color(LIMINE_COLOR_LIGHT_BLUE, "============================================\n\n");

    printf_color(LIMINE_COLOR_AMBER, "[1] kernel.version():\n");
    mp_luos_exec_str("import kernel; print(' ', kernel.version())\n");

    printf_color(LIMINE_COLOR_AMBER, "[2] kernel.get_cpu_vendor():\n");
    mp_luos_exec_str("import kernel; print('  CPU:', kernel.get_cpu_vendor())\n");

    printf_color(LIMINE_COLOR_AMBER, "[3] kernel.get_time():\n");
    mp_luos_exec_str("import kernel; t = kernel.get_time(); print('  Time: {:02d}:{:02d}:{:02d}'.format(t['hours'], t['minutes'], t['seconds']))\n");

    printf_color(LIMINE_COLOR_AMBER, "[4] kernel.get_ram_mb():\n");
    mp_luos_exec_str("import kernel; r = kernel.get_ram_mb(); print('  Total:', r['total_mb'], 'MB'); print('  Free: ', r['free_mb'],  'MB'); print('  Used: ', r['used_mb'],  'MB')\n");

    printf_color(LIMINE_COLOR_AMBER, "[5] kernel.get_heap_stats():\n");
    mp_luos_exec_str("import kernel; h = kernel.get_heap_stats(); print('  Heap total:', h['total'], 'bytes'); print('  Heap used: ', h['used'],  'bytes'); print('  Heap free: ', h['free'],  'bytes')\n");

    printf_color(LIMINE_COLOR_AMBER, "[6] kernel.get_tsc_ms():\n");
    mp_luos_exec_str("import kernel; ms = kernel.get_tsc_ms(); print('  Uptime:', ms, 'ms')\n");

    printf_color(LIMINE_COLOR_AMBER, "[7] CPU feature flags:\n");
    mp_luos_exec_str("import kernel; print('  APIC:', kernel.has_apic()); print('  SSE: ', kernel.has_sse()); print('  AVX: ', kernel.has_avx())\n");

    printf_color(LIMINE_COLOR_AMBER, "[8] kernel.kmalloc_test(256):\n");
    mp_luos_exec_str("import kernel; addr = kernel.kmalloc_test(256)\nif addr != 0:\n    print('  [PASS] allocated at 0x{:X}'.format(addr))\nelse:\n    print('  [FAIL] kmalloc returned 0')\n");

    printf_color(LIMINE_COLOR_AMBER, "[9] kernel.print / kernel.print_color:\n");
    mp_luos_exec_str("import kernel; kernel.print('  Hello from kernel.print!')\n");
    mp_luos_exec_str("import kernel; kernel.print_color('  Colored text (green)!', 0, 255, 0)\n");
    mp_luos_exec_str("import kernel; kernel.print_color('  Colored text (cyan)!',  0, 255, 255)\n");

    printf_color(LIMINE_COLOR_AMBER, "[10] kernel.sleep_ms(100) [TSC]:\n");
    mp_luos_exec_str("import kernel\nt0 = kernel.get_tsc_ms()\nkernel.sleep_ms(100)\nt1 = kernel.get_tsc_ms()\ndelta = t1 - t0\nprint('  Slept ~', delta, 'ms (expected ~100)')\nif 80 <= delta <= 200:\n    print('  [PASS]')\nelse:\n    print('  [WARN] delta out of expected range')\n");

    printf_color(LIMINE_COLOR_AMBER, "[11] kernel.help():\n");
    mp_luos_exec_str("import kernel; kernel.help()\n");

    printf_color(LIMINE_COLOR_LIGHT_BLUE, "============================================\n");
    printf_color(LIMINE_COLOR_LIGHT_GREEN, "     kernel.* bindings test complete.\n");
    printf_color(LIMINE_COLOR_LIGHT_BLUE, "============================================\n\n");
}

static void print_cpu_feature(struct limine_video_driver* video, const char* name, int has_feature) {
    video->printf("  ", LIMINE_COLOR_LIGHT_GRAY);
    video->printf(name, LIMINE_COLOR_WHITE);
    video->printf(": ", LIMINE_COLOR_LIGHT_GRAY);
    video->printf(has_feature ? "YES" : "NO",
                  has_feature ? LIMINE_COLOR_LIGHT_GREEN : LIMINE_COLOR_LIGHT_RED);
    video->printf("\n", 0);
}

static void show_cpu_info(struct limine_video_driver* video) {
    char vendor[13];
    cpuid_get_vendor(vendor);
    vendor[12] = '\0';

    video->printf("\n=== CPU Information ===\n", LIMINE_COLOR_LIGHT_BLUE);
    video->printf("Vendor: ", LIMINE_COLOR_WHITE);
    video->printf(vendor, LIMINE_COLOR_LIGHT_MAGENTA);
    video->printf("\n", 0);

    struct cpuid_result info = cpuid_get_processor_info();
    uint8_t family, model, stepping;
    cpuid_get_family_model(&family, &model, &stepping);

    char fam_str[3] = { '0' + (family / 10), '0' + (family % 10), '\0' };
    char mod_str[3] = { '0' + (model / 10), '0' + (model % 10), '\0' };
    char stp_str[3] = { '0' + (stepping/10), '0' + (stepping% 10), '\0' };

    video->printf("Family: ", LIMINE_COLOR_WHITE); video->printf(fam_str, LIMINE_COLOR_AMBER);
    video->printf("  Model: ", LIMINE_COLOR_WHITE); video->printf(mod_str, LIMINE_COLOR_AMBER);
    video->printf("  Stepping: ",LIMINE_COLOR_WHITE); video->printf(stp_str, LIMINE_COLOR_AMBER);
    video->printf("\n\nFeatures:\n", LIMINE_COLOR_LIGHT_BLUE);

    print_cpu_feature(video, "APIC", cpuid_has_apic());
    print_cpu_feature(video, "TSC", cpuid_has_tsc());
    print_cpu_feature(video, "MSR", cpuid_has_msr());
    print_cpu_feature(video, "x2APIC", cpuid_has_x2apic());
    print_cpu_feature(video, "Invariant TSC", cpuid_has_invariant_tsc());
    print_cpu_feature(video, "MMX", (info.edx & CPUID_FEATURE_MMX) != 0);
    print_cpu_feature(video, "SSE", (info.edx & CPUID_FEATURE_SSE) != 0);
    print_cpu_feature(video, "SSE2", (info.edx & CPUID_FEATURE_SSE2) != 0);
    print_cpu_feature(video, "SSE3", (info.ecx & CPUID_FEATURE_SSE3) != 0);
    print_cpu_feature(video, "SSSE3", (info.ecx & CPUID_FEATURE_SSSE3) != 0);
    print_cpu_feature(video, "SSE4.1", (info.ecx & CPUID_FEATURE_SSE4_1) != 0);
    print_cpu_feature(video, "SSE4.2", (info.ecx & CPUID_FEATURE_SSE4_2) != 0);
    print_cpu_feature(video, "AES", (info.ecx & CPUID_FEATURE_AES) != 0);
    print_cpu_feature(video, "AVX", (info.ecx & CPUID_FEATURE_AVX) != 0);
    video->printf("========================\n\n", LIMINE_COLOR_LIGHT_BLUE);
}

static void show_pci_info(struct limine_video_driver* video, struct apic_driver* apic) {
    video->printf("\n=== PCI Devices List ===\n", LIMINE_COLOR_LIGHT_BLUE);
    char hex[] = "0123456789ABCDEF";
    uintptr_t* pci_slots = (uintptr_t*)device_table[PCI_DEVICE];

    int count = pci_get_device_count();
    if (count > MAX_PCI_DEVICES) count = MAX_PCI_DEVICES;

    for (int i = 0; i < count; i++) {
        uintptr_t dev_addr = pci_slots[i];
        if (dev_addr == 0) continue;

        if (apic) apic->sleep_apic_ms(5);
        if (dev_addr < 0x100000 || (dev_addr % 2 != 0)) {
            video->printf(" [DEAD PTR] Slot:", LIMINE_COLOR_LIGHT_RED);
            char s[4] = { '0' + (i / 10), '0' + (i % 10), ' ', '\0' };
            video->printf(s, LIMINE_COLOR_AMBER);
            video->printf("\n", 0);
            continue;
        }

        struct pci_device* dev = (struct pci_device*)dev_addr;

        char bdf[9] = { hex[(dev->bus >> 4) & 0xF], hex[dev->bus & 0xF], ':',
                        hex[(dev->slot >> 4) & 0xF], hex[dev->slot & 0xF], '.',
                        hex[dev->func & 0xF], ' ', '\0' };
        video->printf(" ", 0);
        video->printf(bdf, LIMINE_COLOR_LIGHT_GREEN);

        video->printf("[", LIMINE_COLOR_LIGHT_GRAY);
        video->printf(get_pci_type_name(dev->class_code), LIMINE_COLOR_CYAN);
        video->printf("] ", LIMINE_COLOR_LIGHT_GRAY);

        char cls[8] = { hex[(dev->class_code >> 4) & 0xF], hex[dev->class_code & 0xF],
                        hex[(dev->subclass >> 4) & 0xF], hex[dev->subclass & 0xF],
                        hex[(dev->prog_if >> 4) & 0xF], hex[dev->prog_if & 0xF],
                        ' ', '\0' };
        video->printf(cls, LIMINE_COLOR_LIGHT_MAGENTA);

        char v_s[5] = { hex[(dev->vendor_id >> 12) & 0xF], hex[(dev->vendor_id >> 8) & 0xF],
                        hex[(dev->vendor_id >> 4) & 0xF], hex[ dev->vendor_id & 0xF], '\0' };
        char d_s[5] = { hex[(dev->device_id >> 12) & 0xF], hex[(dev->device_id >> 8) & 0xF],
                        hex[(dev->device_id >> 4) & 0xF], hex[ dev->device_id & 0xF], '\0' };
        video->printf(v_s, LIMINE_COLOR_AMBER);
        video->printf(":", LIMINE_COLOR_WHITE);
        video->printf(d_s, LIMINE_COLOR_AMBER);
        video->printf("\n", 0);
    }
    video->printf("========================\n", LIMINE_COLOR_LIGHT_BLUE);
}

static void show_usb_controllers(struct limine_video_driver* video) {
    video->printf("\n=== USB Controllers List ===\n", LIMINE_COLOR_LIGHT_BLUE);
    char hex[] = "0123456789ABCDEF";
    uintptr_t* pci_slots = (uintptr_t*)device_table[PCI_DEVICE];
    int found = 0;

    for (int i = 1; i <= MAX_PCI_DEVICES; i++) {
        uintptr_t dev_addr = pci_slots[i];
        if (dev_addr == 0 || dev_addr < 0x100000 || (dev_addr % 2 != 0)) continue;
        struct pci_device* dev = (struct pci_device*)dev_addr;
        if (dev->class_code != 0x0C || dev->subclass != 0x03) continue;

        found = 1;
        video->printf(" [", LIMINE_COLOR_LIGHT_GRAY);
        video->printf(get_usb_type_name(dev->prog_if), LIMINE_COLOR_CYAN);
        video->printf("] Vendor:", LIMINE_COLOR_WHITE);

        char v_s[5] = { hex[(dev->vendor_id >> 12) & 0xF], hex[(dev->vendor_id >> 8) & 0xF],
                        hex[(dev->vendor_id >> 4) & 0xF], hex[ dev->vendor_id & 0xF], '\0' };
        char d_s[5] = { hex[(dev->device_id >> 12) & 0xF], hex[(dev->device_id >> 8) & 0xF],
                        hex[(dev->device_id >> 4) & 0xF], hex[ dev->device_id & 0xF], '\0' };
        video->printf(v_s, LIMINE_COLOR_AMBER);
        video->printf(" Device:", LIMINE_COLOR_WHITE);
        video->printf(d_s, LIMINE_COLOR_AMBER);
        video->printf("\n", 0);
    }

    if (!found)
        video->printf(" No USB controllers found on PCI bus.\n", LIMINE_COLOR_LIGHT_RED);
    video->printf("============================\n", LIMINE_COLOR_LIGHT_BLUE);
}

static void cmd_usb_ports(struct limine_video_driver* video) {
    video->printf("\n=== USB Ports Diagnostic v3 ===\n", LIMINE_COLOR_CYAN);

    struct root_hub hub;
    int total_ports = 0;

    int types[] = { USB_TYPE_UHCI, USB_TYPE_OHCI, USB_TYPE_EHCI, USB_TYPE_XHCI };
    const char* type_names[] = { "UHCI", "OHCI", "EHCI", "XHCI" };

    for (int t = 0; t < 4; t++) {
        void* drv_ptr = get_self_driver(USB_DRIVER, types[t]);
        if (!drv_ptr) continue;

        video->printf("Checking ", LIMINE_COLOR_WHITE);
        video->printf(type_names[t], LIMINE_COLOR_CYAN);
        video->printf("...\n", LIMINE_COLOR_WHITE);

        struct { int (*get_count)(void); void* (*get_ctrl)(int); } *gdrv = drv_ptr;
        int c_count = gdrv->get_count();

        for (int i = 0; i < c_count; i++) {
            void* ctrl = gdrv->get_ctrl(i);
            if (!ctrl) continue;

            if (root_hub_init(&hub, types[t], ctrl)) {
                video->printf("  Ctrl #", LIMINE_COLOR_LIGHT_GRAY);
                char cnum[2] = { '0' + i, '\0' };
                video->printf(cnum, LIMINE_COLOR_AMBER);
                video->printf(": Ports Found: ", LIMINE_COLOR_LIGHT_GRAY);
                int pc = hub.port_count;
                char pcount[3] = { '0' + (pc / 10), '0' + (pc % 10), '\0' };
                video->printf(pcount, LIMINE_COLOR_LIGHT_GREEN);
                video->printf("\n", 0);

                for (uint8_t p = 1; p <= hub.port_count; p++) {
                    uint32_t status = root_hub_port_status(&hub, p);
                    if (!(status & 0x01)) continue;

                    video->printf("    Port ", LIMINE_COLOR_DARK_GRAY);
                    char pnum[3] = { '0' + p, ':', '\0' };
                    video->printf(pnum, LIMINE_COLOR_AMBER);
                    video->printf(" [CONNECTED] 0x", LIMINE_COLOR_LIGHT_GREEN);
                    char hex_stat[9];
                    for (int j = 0; j < 8; j++) {
                        int nib = (status >> ((7 - j) * 4)) & 0xF;
                        hex_stat[j] = (nib < 10) ? '0' + nib : 'A' + (nib - 10);
                    }
                    hex_stat[8] = '\0';
                    video->printf(hex_stat, LIMINE_COLOR_AMBER);
                    video->printf("\n", 0);
                }
                total_ports += hub.port_count;
            } else {
                video->printf("  Ctrl #? : Hub Init Failed\n", LIMINE_COLOR_LIGHT_RED);
            }
        }
    }

    if (total_ports == 0) {
        video->printf("\n!!! NO PORTS DETECTED !!!\n", LIMINE_COLOR_LIGHT_RED);
        video->printf("Check if PCI Bus Master & Memory Space are enabled.\n", LIMINE_COLOR_AMBER);
    }
}

static void cmd_usb_list(struct limine_video_driver* video) {
    video->printf("\n=== Connected USB Devices ===\n", LIMINE_COLOR_CYAN);
    char hex[] = "0123456789ABCDEF";
    int found_count = 0;

    for (int i = 0; i < MAX_USB_DEVICES; i++) {
        struct usb_device* dev = (struct usb_device*)device_table[USB_DEVICE][i];
        if (!dev) continue;

        found_count++;
        video->printf(" #", LIMINE_COLOR_LIGHT_GRAY);
        char dnum[3] = { '0' + (found_count / 10), '0' + (found_count % 10), '\0' };
        video->printf(dnum, LIMINE_COLOR_AMBER);
        video->printf(" ", 0);
        video->printf(dev->vendor_str[0] ? dev->vendor_str : "Unknown", LIMINE_COLOR_LIGHT_GREEN);
        video->printf(" ", 0);
        video->printf(dev->product_str[0] ? dev->product_str : "Device", LIMINE_COLOR_LIGHT_CYAN);

        video->printf("\n    [Port:", LIMINE_COLOR_WHITE);
        char port_s[3] = { '0' + (dev->port / 10), '0' + (dev->port % 10), '\0' };
        char addr_s[3] = { '0' + (dev->address / 10), '0' + (dev->address % 10), '\0' };
        video->printf(port_s, LIMINE_COLOR_AMBER);
        video->printf(" Addr:", LIMINE_COLOR_WHITE);
        video->printf(addr_s, LIMINE_COLOR_AMBER);
        video->printf("] ID ", LIMINE_COLOR_LIGHT_GRAY);

        char vid_s[5] = { hex[(dev->desc.idVendor >> 12) & 0xF], hex[(dev->desc.idVendor >> 8) & 0xF],
                          hex[(dev->desc.idVendor >> 4) & 0xF], hex[ dev->desc.idVendor & 0xF], '\0' };
        char pid_s[5] = { hex[(dev->desc.idProduct >> 12) & 0xF], hex[(dev->desc.idProduct >> 8) & 0xF],
                          hex[(dev->desc.idProduct >> 4) & 0xF], hex[ dev->desc.idProduct & 0xF], '\0' };
        video->printf(vid_s, LIMINE_COLOR_LIGHT_GREEN);
        video->printf(":", LIMINE_COLOR_WHITE);
        video->printf(pid_s, LIMINE_COLOR_LIGHT_GREEN);

        char cls_s[3] = { hex[(dev->desc.bDeviceClass >> 4) & 0xF], hex[dev->desc.bDeviceClass & 0xF], '\0' };
        char sub_s[3] = { hex[(dev->desc.bDeviceSubClass >> 4) & 0xF], hex[dev->desc.bDeviceSubClass & 0xF], '\0' };
        char prt_s[3] = { hex[(dev->desc.bDeviceProtocol >> 4) & 0xF], hex[dev->desc.bDeviceProtocol & 0xF], '\0' };
        video->printf("\n    Class: ", LIMINE_COLOR_LIGHT_GRAY); video->printf(cls_s, LIMINE_COLOR_AMBER);
        video->printf(" Sub: ", LIMINE_COLOR_LIGHT_GRAY); video->printf(sub_s, LIMINE_COLOR_AMBER);
        video->printf(" Prot: ", LIMINE_COLOR_LIGHT_GRAY); video->printf(prt_s, LIMINE_COLOR_AMBER);
        video->printf(" Type: ", LIMINE_COLOR_LIGHT_GRAY);

        uint8_t cls = dev->desc.bDeviceClass;
        if (cls == 0x03) video->printf("HID", LIMINE_COLOR_LIGHT_CYAN);
        else if (cls == 0x09) video->printf("HUB", LIMINE_COLOR_LIGHT_MAGENTA);
        else if (cls == 0x08) video->printf("Storage", LIMINE_COLOR_LIGHT_BLUE);
        else if (cls == 0x00) video->printf("Defined in Interface", LIMINE_COLOR_LIGHT_GRAY);
        else video->printf("Other", LIMINE_COLOR_DARK_GRAY);

        video->printf("\n", 0);
    }

    if (found_count == 0)
        video->printf(" No USB devices found.\n", LIMINE_COLOR_LIGHT_RED);

    printf_color(0xFF888888, " hotplug: %llu connects, %llu disconnects, %llu polls, %llu rescans\n",
                 (unsigned long long)usb_hotplug_connect_count(),
                 (unsigned long long)usb_hotplug_disconnect_count(),
                 (unsigned long long)usb_hotplug_poll_count(),
                 (unsigned long long)usb_hotplug_rescan_count());

    video->printf("==============================\n", LIMINE_COLOR_CYAN);
}

#define SCHEDULER_TEST_DEFAULT_TASKS 4
#define SCHEDULER_TEST_DEFAULT_ITERS 15
#define SCHEDULER_TEST_MAX_TASKS 64
#define SCHEDULER_TEST_WORK_ROUNDS 20000

static volatile long scheduler_test_counter = 0;
static spinlock_t scheduler_test_lock = SPINLOCK_INIT;
static volatile int scheduler_test_done = 0;
static volatile int scheduler_test_corrupt = 0;
static volatile uint8_t scheduler_test_core_seen[MAX_CORES];
static int scheduler_test_iters = SCHEDULER_TEST_DEFAULT_ITERS;

static uint32_t scheduler_test_hash(uint32_t seed, int rounds) {
    uint32_t x = seed | 1;
    for (int i = 0; i < rounds; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
    }
    return x;
}

static void scheduler_test_worker(void *arg) {
    long id = (long)arg;

    for (int i = 0; i < scheduler_test_iters; i++) {
        size_t buf_size = 256 + (size_t)((id * 131 + i * 977) % 3072);
        uint8_t *buf = kmalloc(buf_size);
        uint32_t seed = (uint32_t)(id * 2654435761u + i * 40503u + 1);

        for (size_t j = 0; j < buf_size; j++)
            buf[j] = (uint8_t)((seed >> (j % 24)) ^ j);

        uint32_t checksum = 0;
        for (size_t j = 0; j < buf_size; j++)
            checksum = (checksum * 31) + buf[j];

        uint32_t work = scheduler_test_hash(seed, SCHEDULER_TEST_WORK_ROUNDS);

        uint32_t verify = 0;
        for (size_t j = 0; j < buf_size; j++)
            verify = (verify * 31) + buf[j];

        if (verify != checksum)
            __atomic_fetch_add(&scheduler_test_corrupt, 1, __ATOMIC_SEQ_CST);

        kfree(buf);

        spin_lock(&scheduler_test_lock);
        scheduler_test_counter++;
        spin_unlock(&scheduler_test_lock);

        scheduler_test_core_seen[current_core()] = 1;

        printf_color(LIMINE_COLOR_AMBER,
                     "[scheduler-test] task %d iter %d/%d on core %d (tid=%d, work=%08x)\n",
                     (int)id, i + 1, scheduler_test_iters, current_core(),
                     (int)current_task()->tid, (unsigned)work);

        scheduler_sleep_ms(10);
    }

    __atomic_fetch_add(&scheduler_test_done, 1, __ATOMIC_SEQ_CST);
    task_exit();
}

static void cmd_scheduler_test(struct limine_video_driver *video, const char *args)
{
    int req_cores = scheduler_core_count();
    int req_tasks = SCHEDULER_TEST_DEFAULT_TASKS;
    int req_iters = SCHEDULER_TEST_DEFAULT_ITERS;

    const char *p = args;
    char *end;

    while (*p == ' ') p++;
    if (*p) {
        long v = strtol(p, &end, 10);
        if (end != p) { req_cores = (int)v; p = end; }
    }
    while (*p == ' ') p++;
    if (*p) {
        long v = strtol(p, &end, 10);
        if (end != p) { req_tasks = (int)v; p = end; }
    }
    while (*p == ' ') p++;
    if (*p) {
        long v = strtol(p, &end, 10);
        if (end != p) { req_iters = (int)v; p = end; }
    }

    if (req_cores < 1) req_cores = 1;
    if (req_cores > scheduler_core_count()) req_cores = scheduler_core_count();
    if (req_tasks < 1) req_tasks = 1;
    if (req_tasks > SCHEDULER_TEST_MAX_TASKS) req_tasks = SCHEDULER_TEST_MAX_TASKS;
    if (req_iters < 1) req_iters = 1;

    scheduler_test_iters = req_iters;

    video->printf("\n=== Scheduler Test ===\n", LIMINE_COLOR_CYAN);
    printf_color(LIMINE_COLOR_LIGHT_GRAY, " cores online: %d, using: %d\n",
                 scheduler_core_count(), req_cores);
    printf_color(LIMINE_COLOR_LIGHT_GRAY,
                 " spawning %d tasks, %d iterations each (heap alloc + hash workload)...\n\n",
                 req_tasks, req_iters);

    scheduler_test_counter = 0;
    scheduler_test_done = 0;
    scheduler_test_corrupt = 0;
    for (int i = 0; i < MAX_CORES; i++) scheduler_test_core_seen[i] = 0;

    for (long i = 0; i < req_tasks; i++) {
        char name[16];
        snprintf(name, sizeof(name), "shed-test%d", (int)i);
        int target_core = (int)(i % req_cores);
        task_create_on_core(name, scheduler_test_worker, (void *)i, PRIO_DEFAULT, target_core);
    }

    while (__atomic_load_n(&scheduler_test_done, __ATOMIC_SEQ_CST) < req_tasks)
        scheduler_yield();

    long expected = (long)req_tasks * req_iters;

    video->printf("\n", 0);
    printf_color(LIMINE_COLOR_WHITE, " counter = %d (expected %d) -> ",
                 (int)scheduler_test_counter, (int)expected);

    if (scheduler_test_counter == expected && scheduler_test_corrupt == 0) {
        video->printf("PASS\n", LIMINE_COLOR_LIGHT_GREEN);
    } else if (scheduler_test_corrupt != 0) {
        printf_color(LIMINE_COLOR_LIGHT_RED,
                     "FAIL (%d buffer corruption(s) detected)\n", scheduler_test_corrupt);
    } else {
        video->printf("FAIL (lost updates under concurrency)\n", LIMINE_COLOR_LIGHT_RED);
    }

    printf_color(LIMINE_COLOR_LIGHT_GRAY, " cores that actually ran a task: ");
    for (int i = 0; i < MAX_CORES; i++) {
        if (scheduler_test_core_seen[i])
            printf_color(LIMINE_COLOR_AMBER, "%d ", i);
    }
    video->printf("\n======================\n\n", LIMINE_COLOR_CYAN);
}

static void cmd_hid_test(struct limine_video_driver *video, struct keyboard_driver *kbd, struct mouse_driver *mouse, struct tsc_driver *tsc)
{
    video->printf("\n=== HID Simultaneous Test (Keyboard + Mouse) ===\n",
                  LIMINE_COLOR_CYAN);

    if (!kbd) {
        video->printf(" Keyboard: NOT FOUND\n", LIMINE_COLOR_LIGHT_RED);
    } else {
        int kt = kbd->get_active_type();
        video->printf(" Keyboard: ", LIMINE_COLOR_LIGHT_GRAY);
        if (kt == USB_KEYBOARD) video->printf("USB\n", LIMINE_COLOR_LIGHT_GREEN);
        else if (kt == PS2_KEYBOARD) video->printf("PS/2\n", LIMINE_COLOR_LIGHT_GREEN);
        else video->printf("None\n", LIMINE_COLOR_AMBER);
    }

    if (!mouse) {
        video->printf(" Mouse:    NOT FOUND\n", LIMINE_COLOR_LIGHT_RED);
    } else {
        int mt = mouse->get_active_type();
        video->printf(" Mouse:    ", LIMINE_COLOR_LIGHT_GRAY);
        if (mt == USB_MOUSE) video->printf("USB\n", LIMINE_COLOR_LIGHT_GREEN);
        else if (mt == PS2_MOUSE) video->printf("PS/2\n", LIMINE_COLOR_LIGHT_GREEN);
        else video->printf("None\n", LIMINE_COLOR_AMBER);
    }

    if (!kbd && !mouse) {
        video->printf(" No input devices found. Aborting.\n",
                      LIMINE_COLOR_LIGHT_RED);
        video->printf("================================================\n\n",
                      LIMINE_COLOR_CYAN);
        return;
    }

    mouse_sample_t hid_sample;
    if (mouse) mouse_updater_take(&hid_sample);

    video->printf("\n", 0);
    video->printf(" Type keys (ESC = exit) and move the mouse simultaneously.\n",
                  LIMINE_COLOR_WHITE);
    video->printf(" The test validates both USB paths receive events concurrently.\n",
                  LIMINE_COLOR_LIGHT_GRAY);
    video->printf(" Test runs for 15 seconds or until ESC is pressed.\n\n",
                  LIMINE_COLOR_DARK_GRAY);

    int keys_received = 0;
    int mouse_events = 0;
    int32_t total_dx = 0;
    int32_t total_dy = 0;
    int btn_events = 0;

    uint64_t start_tsc = tsc->get_tsc_uptime_ms();
    int running = 1;
    while (running) {

        if (kbd) {
            kbd->keyboard_handler();

            while (kbd->has_key()) {
                uint8_t sc = kbd->get_key();

                if (kbd->scancode_to_key_code(sc) == KEY_ESC) { running = 0; break; }
                char c = kbd->scancode_to_char(sc);
                keys_received++;
                video->printf(" [KBD] key=0x", LIMINE_COLOR_LIGHT_CYAN);

                const char *hx = "0123456789ABCDEF";
                char hs[3] = { hx[(sc >> 4) & 0xF], hx[sc & 0xF], '\0' };
                video->printf(hs, LIMINE_COLOR_AMBER);
                if (c >= 32 && c <= 126) {
                    char cs[3] = { ' ', c, '\0' };
                    video->printf(" char='", LIMINE_COLOR_LIGHT_GRAY);
                    video->printf(cs + 1, LIMINE_COLOR_WHITE);
                    video->printf("'", LIMINE_COLOR_LIGHT_GRAY);
                }
                video->printf("\n", 0);
            }
        }

        if (mouse) {
            if (mouse_updater_take(&hid_sample)) {
                int32_t dx = hid_sample.dx;
                int32_t dy = hid_sample.dy;
                int any_btn = hid_sample.btn_left || hid_sample.btn_right || hid_sample.btn_middle;

                total_dx += dx;
                total_dy += dy;
                mouse_events++;
                if (any_btn) btn_events++;

                video->printf(" [MSE] dx=", LIMINE_COLOR_LIGHT_MAGENTA);
                if (dx < 0) { video->printf("-", LIMINE_COLOR_LIGHT_RED); dx = -dx; }
                print_u64(video, (uint64_t)dx, LIMINE_COLOR_AMBER);
                video->printf(" dy=", LIMINE_COLOR_LIGHT_MAGENTA);
                if (dy < 0) { video->printf("-", LIMINE_COLOR_LIGHT_RED); dy = -dy; }
                print_u64(video, (uint64_t)dy, LIMINE_COLOR_AMBER);
                if (hid_sample.btn_left) video->printf(" L", LIMINE_COLOR_LIGHT_GREEN);
                if (hid_sample.btn_right) video->printf(" R", LIMINE_COLOR_LIGHT_GREEN);
                if (hid_sample.btn_middle) video->printf(" M", LIMINE_COLOR_LIGHT_GREEN);
                video->printf("\n", 0);
            }
        }

        if ((tsc->get_tsc_uptime_ms() - start_tsc) >= 10000)
            running = 0;

        asm volatile("pause");
    }

    video->printf("\n--- HID Test Summary ---\n", LIMINE_COLOR_LIGHT_BLUE);

    video->printf(" Keys received:   ", LIMINE_COLOR_LIGHT_GRAY);
    print_u64(video, (uint64_t)keys_received, LIMINE_COLOR_AMBER);
    video->printf("\n", 0);

    video->printf(" Mouse events:    ", LIMINE_COLOR_LIGHT_GRAY);
    print_u64(video, (uint64_t)mouse_events, LIMINE_COLOR_AMBER);
    video->printf("\n", 0);

    video->printf(" Button events:   ", LIMINE_COLOR_LIGHT_GRAY);
    print_u64(video, (uint64_t)btn_events, LIMINE_COLOR_AMBER);
    video->printf("\n", 0);

    video->printf(" Total mouse dX=", LIMINE_COLOR_LIGHT_GRAY);
    if (total_dx < 0) { video->printf("-", LIMINE_COLOR_LIGHT_RED); total_dx = -total_dx; }
    print_u64(video, (uint64_t)total_dx, LIMINE_COLOR_AMBER);
    video->printf(" dY=", LIMINE_COLOR_LIGHT_GRAY);
    if (total_dy < 0) { video->printf("-", LIMINE_COLOR_LIGHT_RED); total_dy = -total_dy; }
    print_u64(video, (uint64_t)total_dy, LIMINE_COLOR_AMBER);
    video->printf("\n", 0);

    video->printf(" Result: ", LIMINE_COLOR_LIGHT_GRAY);
    int ok = (keys_received > 0 || mouse_events > 0);
    video->printf(ok ? "[PASS] Input received on at least one device.\n"
                     : "[WARN] No input detected (devices connected?).\n",
                  ok ? LIMINE_COLOR_LIGHT_GREEN : LIMINE_COLOR_AMBER);

    video->printf("================================================\n\n",
                  LIMINE_COLOR_CYAN);
}

static void cmd_kbd_test(struct limine_video_driver* video, struct keyboard_driver* kbd) {
    int type = kbd->get_active_type();
    video->printf("\n=== Keyboard Test (", LIMINE_COLOR_CYAN);
    video->printf(type == USB_KEYBOARD ? "USB" : "PS/2", LIMINE_COLOR_AMBER);
    video->printf(") ===\n", LIMINE_COLOR_CYAN);

    char buf[128];
    kbd->input("luos # ", buf, 128, LIMINE_COLOR_LIGHT_CYAN, video->printf);
}

static void cmd_hpet(struct limine_video_driver* video) {
    video->printf("\n=== HPET Timer ===\n", LIMINE_COLOR_LIGHT_BLUE);

    struct hpet_driver* hpet = (struct hpet_driver*)get_self_driver(TIMER_DRIVER, HPET_TIMER);

    if (!hpet) {
        video->printf(" Status:    ", LIMINE_COLOR_LIGHT_GRAY);
        video->printf("NOT PRESENT\n", LIMINE_COLOR_LIGHT_RED);
        video->printf(" HPET hardware not found or init failed.\n", LIMINE_COLOR_DARK_GRAY);
        video->printf("==================\n\n", LIMINE_COLOR_LIGHT_BLUE);
        return;
    }

    if (!hpet->is_hpet_available()) {
        video->printf(" Status:    ", LIMINE_COLOR_LIGHT_GRAY);
        video->printf("UNAVAILABLE\n", LIMINE_COLOR_LIGHT_RED);
        video->printf(" Driver loaded but counter is not ticking.\n", LIMINE_COLOR_DARK_GRAY);
        video->printf("==================\n\n", LIMINE_COLOR_LIGHT_BLUE);
        return;
    }

    video->printf(" Status:    ", LIMINE_COLOR_LIGHT_GRAY);
    video->printf("OK\n", LIMINE_COLOR_LIGHT_GREEN);

    video->printf(" Counter:   ", LIMINE_COLOR_LIGHT_GRAY);
    video->printf(hpet->is_hpet_64bit() ? "64-bit\n" : "32-bit\n", LIMINE_COLOR_AMBER);

    video->printf(" Timers:    ", LIMINE_COLOR_LIGHT_GRAY);
    uint32_t tcnt = hpet->get_hpet_timer_count();
    char tc_s[3] = { '0' + (tcnt / 10), '0' + (tcnt % 10), '\0' };
    video->printf(tc_s, LIMINE_COLOR_AMBER);
    video->printf("\n", 0);

    video->printf(" Frequency: ", LIMINE_COLOR_LIGHT_GRAY);
    print_u64(video, hpet->get_hpet_frequency(), LIMINE_COLOR_AMBER);
    video->printf(" Hz\n", LIMINE_COLOR_LIGHT_GRAY);

    video->printf(" Counter:   ", LIMINE_COLOR_LIGHT_GRAY);
    print_u64(video, hpet->get_hpet_counter(), LIMINE_COLOR_TURQUOISE);
    video->printf("\n", 0);

    video->printf(" Uptime:    ", LIMINE_COLOR_LIGHT_GRAY);
    print_u64(video, hpet->get_hpet_ms(), LIMINE_COLOR_AMBER);
    video->printf(" ms\n", LIMINE_COLOR_LIGHT_GRAY);

    video->printf(" Self-test: ", LIMINE_COLOR_LIGHT_GRAY);
    uint64_t t0 = hpet->get_hpet_counter();
    hpet->sleep_hpet_ms(10);
    uint64_t t1 = hpet->get_hpet_counter();

    if (t1 > t0) {
        video->printf("PASS", LIMINE_COLOR_LIGHT_GREEN);
        video->printf(" (delta=", LIMINE_COLOR_DARK_GRAY);
        print_u64(video, t1 - t0, LIMINE_COLOR_DARK_GRAY);
        video->printf(" ticks)\n", LIMINE_COLOR_DARK_GRAY);
    } else {
        video->printf("FAIL", LIMINE_COLOR_LIGHT_RED);
        video->printf(" (counter did not advance)\n", LIMINE_COLOR_DARK_GRAY);
    }

    video->printf("==================\n\n", LIMINE_COLOR_LIGHT_BLUE);
}

static void guess_number_game(struct limine_video_driver* video, struct rtc_driver* rtc, struct keyboard_driver* kbd, struct apic_driver* apic)
{
    (void)apic;
    video->printf("\n=== GUESS THE NUMBER ===\n", LIMINE_COLOR_LIGHT_MAGENTA);
    video->printf("I'm thinking of a number between 1 and 3.\n", LIMINE_COLOR_WHITE);
    video->printf("Can you guess it? You have 3 tries!\n", LIMINE_COLOR_LIGHT_GRAY);

    struct system_time* time = rtc->get_rtc_time();
    int secret_number = (time->seconds % 3) + 1;
    char guess_buffer[16];
    int attempts = 3;
    int guessed = 0;

    while (attempts > 0 && !guessed) {
        char prompt[12] = { '[', '0' + attempts, ']', ' ', 'G', 'u', 'e', 's', 's', ':', ' ', '\0' };
        kbd->input(prompt, guess_buffer, 16, LIMINE_COLOR_CYAN, video->printf);

        if (guess_buffer[0] >= '1' && guess_buffer[0] <= '3' && guess_buffer[1] == '\0') {
            int guess = guess_buffer[0] - '0';
            if (guess == secret_number) {
                video->printf(" CORRECT! You win!\n", LIMINE_COLOR_LIGHT_GREEN);
                guessed = 1;
            } else {
                video->printf(" Wrong! ", LIMINE_COLOR_LIGHT_RED);
                attempts--;
                if (attempts > 0)
                    video->printf(guess < secret_number ? "Try higher.\n" : "Try lower.\n",
                                  LIMINE_COLOR_AMBER);
            }
        } else {
            video->printf("Please enter 1, 2 or 3.\n", LIMINE_COLOR_LIGHT_RED);
        }
    }

    if (!guessed) {
        video->printf("Game over! The number was ", LIMINE_COLOR_LIGHT_RED);
        char n[2] = { '0' + secret_number, '\0' };
        video->printf(n, LIMINE_COLOR_AMBER);
        video->printf("\n", 0);
    }

    video->printf("\nThanks for playing! Type 'game' to play again.\n", LIMINE_COLOR_LIGHT_GRAY);
    video->printf("=======================\n\n", LIMINE_COLOR_LIGHT_MAGENTA);
}

static void handle_repeat_command(struct limine_video_driver* video, const char* cmd_buffer) {
    const char* ptr = cmd_buffer + 7;
    unsigned int count = parse_number(ptr);

    if (count == 0 || count > 100) {
        video->printf(count == 0 ? "Usage: repeat <count> <text>\n"
                                 : "Count too large, maximum is 100\n",
                      LIMINE_COLOR_LIGHT_RED);
        return;
    }

    while (*ptr >= '0' && *ptr <= '9') ptr++;
    while (*ptr == ' ') ptr++;
    if (*ptr == '\0') { video->printf("Usage: repeat <count> <text>\n", LIMINE_COLOR_LIGHT_RED); return; }

    video->printf("Repeating '", LIMINE_COLOR_WHITE);
    video->printf(ptr, LIMINE_COLOR_AMBER);
    video->printf("' ", LIMINE_COLOR_WHITE);

    char count_str[16];
    char* p = count_str;
    unsigned int n = count;
    do { *p++ = '0' + (n % 10); n /= 10; } while (n > 0);
    *p-- = '\0';
    for (char* q = count_str; q < p; q++, p--) { char tmp = *q; *q = *p; *p = tmp; }

    video->printf(count_str, LIMINE_COLOR_CYAN);
    video->printf(" times:\n", LIMINE_COLOR_WHITE);

    for (unsigned int i = 0; i < count; i++) {
        char line_num[16];
        p = line_num; n = i + 1;
        do { *p++ = '0' + (n % 10); n /= 10; } while (n > 0);
        *p-- = '\0';
        for (char* q = line_num; q < p; q++, p--) { char tmp = *q; *q = *p; *p = tmp; }

        video->printf("[", LIMINE_COLOR_LIGHT_GRAY);
        video->printf(line_num, LIMINE_COLOR_CYAN);
        video->printf("] ", LIMINE_COLOR_LIGHT_GRAY);
        video->printf(ptr, LIMINE_COLOR_WHITE);
        video->printf("\n", 0);
    }
}

static void print_ram_line(struct limine_video_driver* video, const char* label, uint64_t mb, uint32_t val_color) {
    video->printf(label, LIMINE_COLOR_LIGHT_GRAY);
    uint64_t gb_int = mb / 1024;
    uint64_t gb_frac = (mb % 1024) * 10 / 1024;
    print_u64(video, gb_int, val_color);
    video->printf(".", val_color);
    print_u64(video, gb_frac, val_color);
    video->printf(" GB (", LIMINE_COLOR_LIGHT_GRAY);
    print_u64(video, mb, val_color);
    video->printf(" MB)\n", LIMINE_COLOR_LIGHT_GRAY);
}

static void print_ptr_hex(struct limine_video_driver* video, void* ptr, uint32_t color) {
    char hex_chars[] = "0123456789ABCDEF";
    uint64_t val = (uint64_t)(uintptr_t)ptr;
    video->printf("0x", LIMINE_COLOR_DARK_GRAY);
    for (int i = 0; i < 16; i++) {
        char s[2] = { hex_chars[(val >> ((15 - i) * 4)) & 0xF], '\0' };
        video->printf(s, color);
    }
}

static uint64_t session_start_tsc = 0;

static void cmd_ram(struct limine_video_driver* video) {
    video->printf("\n=== Memory System Diagnostic ===\n", LIMINE_COLOR_CYAN);

    uint64_t free_p = pmm_free_page_count();
    uint64_t used_p = pmm_used_pages();
    uint64_t actual_total_p = free_p + used_p;

    uint64_t total_mb = actual_total_p / 256;
    uint64_t free_mb = free_p / 256;
    uint64_t used_mb = used_p / 256;

    video->printf(" RAM Statistics:\n", LIMINE_COLOR_WHITE);
    print_ram_line(video, "  Total Usable: ", total_mb, LIMINE_COLOR_AMBER);
    print_ram_line(video, "  Free Memory:  ", free_mb, LIMINE_COLOR_LIGHT_GREEN);
    print_ram_line(video, "  Used Memory:  ", used_mb, LIMINE_COLOR_LIGHT_RED);

    uint64_t max_addr_p = pmm_total_pages();
    uint64_t max_mb = max_addr_p / 256;
    if (max_mb > total_mb) {
        video->printf("\n Hardware Map:\n", LIMINE_COLOR_WHITE);
        video->printf("  Physical Limit: ", LIMINE_COLOR_DARK_GRAY);
        uint64_t m_gb_i = max_mb / 1024;
        uint64_t m_gb_f = (max_mb % 1024) * 10 / 1024;
        print_u64(video, m_gb_i, LIMINE_COLOR_DARK_GRAY);
        video->printf(".", LIMINE_COLOR_DARK_GRAY);
        print_u64(video, m_gb_f, LIMINE_COLOR_DARK_GRAY);
        video->printf(" GB (includes MMIO/Reserved holes)\n", LIMINE_COLOR_DARK_GRAY);
    }

    video->printf("\n Heap Statistics:\n", LIMINE_COLOR_WHITE);
    video->printf("  Total:  ", LIMINE_COLOR_LIGHT_GRAY);
    print_u64(video, heap_get_total() / 1024, LIMINE_COLOR_AMBER);
    video->printf(" KB\n", LIMINE_COLOR_LIGHT_GRAY);
    video->printf("  Used:   ", LIMINE_COLOR_LIGHT_GRAY);
    print_u64(video, heap_get_used() / 1024, LIMINE_COLOR_LIGHT_RED);
    video->printf(" KB\n", LIMINE_COLOR_LIGHT_GRAY);
    video->printf("  Free:   ", LIMINE_COLOR_LIGHT_GRAY);
    print_u64(video, (heap_get_total() - heap_get_used()) / 1024, LIMINE_COLOR_LIGHT_GREEN);
    video->printf(" KB\n", LIMINE_COLOR_LIGHT_GRAY);

    video->printf("\n Virtual Memory:\n", LIMINE_COLOR_WHITE);
    video->printf("  Kernel PML4: 0x", LIMINE_COLOR_LIGHT_GRAY);
    char hex_chars[] = "0123456789ABCDEF";
    uint64_t pml4 = vmm_kernel_pml4();
    for (int i = 0; i < 16; i++) {
        char s[2] = { hex_chars[(pml4 >> ((15 - i) * 4)) & 0xF], '\0' };
        video->printf(s, LIMINE_COLOR_AMBER);
    }
    video->printf("\n", LIMINE_COLOR_LIGHT_GRAY);

    video->printf("\n Pointer Uniqueness Test:\n", LIMINE_COLOR_WHITE);
    void* ptr_a = kmalloc(128);
    void* ptr_b = kmalloc(128);
    video->printf("  ptr_a = ", LIMINE_COLOR_LIGHT_GRAY);
    if (ptr_a) print_ptr_hex(video, ptr_a, LIMINE_COLOR_AMBER);
    else video->printf("NULL", LIMINE_COLOR_LIGHT_RED);
    video->printf("\n", LIMINE_COLOR_LIGHT_GRAY);
    video->printf("  ptr_b = ", LIMINE_COLOR_LIGHT_GRAY);
    if (ptr_b) print_ptr_hex(video, ptr_b, LIMINE_COLOR_AMBER);
    else video->printf("NULL", LIMINE_COLOR_LIGHT_RED);
    video->printf("\n", LIMINE_COLOR_LIGHT_GRAY);
    video->printf("  Result: ", LIMINE_COLOR_LIGHT_GRAY);
    if (ptr_a && ptr_b && ptr_a != ptr_b)
        video->printf("[PASS] addresses are unique\n", LIMINE_COLOR_LIGHT_GREEN);
    else if (!ptr_a || !ptr_b)
        video->printf("[FAIL] allocation returned NULL\n", LIMINE_COLOR_LIGHT_RED);
    else
        video->printf("[FAIL] same address!\n", LIMINE_COLOR_LIGHT_RED);
    kfree(ptr_a);
    kfree(ptr_b);

    video->printf("\n kmalloc Loop Test (10 x 64 bytes):\n", LIMINE_COLOR_WHITE);
    void* slots[10];
    int all_ok = 1, unique = 1;
    for (int i = 0; i < 10; i++) {
        slots[i] = kmalloc(64);
        video->printf("  [", LIMINE_COLOR_DARK_GRAY);
        print_u64(video, (uint64_t)i, LIMINE_COLOR_LIGHT_GRAY);
        video->printf("] ", LIMINE_COLOR_DARK_GRAY);
        if (!slots[i]) {
            video->printf("NULL  -- heap exhausted!\n", LIMINE_COLOR_LIGHT_RED);
            all_ok = 0;
            continue;
        }
        print_ptr_hex(video, slots[i], LIMINE_COLOR_AMBER);
        video->printf("  heap_used: ", LIMINE_COLOR_DARK_GRAY);
        print_u64(video, heap_get_used(), LIMINE_COLOR_LIGHT_RED);
        video->printf(" B\n", LIMINE_COLOR_DARK_GRAY);
        for (int j = 0; j < i; j++) {
            if (slots[j] && slots[j] == slots[i]) unique = 0;
        }
    }
    video->printf("  Loop result: ", LIMINE_COLOR_LIGHT_GRAY);
    if (all_ok && unique) video->printf("[PASS] all unique, no NULL\n", LIMINE_COLOR_LIGHT_GREEN);
    else if (!unique) video->printf("[FAIL] duplicate address detected\n", LIMINE_COLOR_LIGHT_RED);
    else video->printf("[FAIL] some allocations returned NULL\n", LIMINE_COLOR_LIGHT_RED);
    for (int i = 0; i < 10; i++) { if (slots[i]) kfree(slots[i]); }
    video->printf("  heap_used after kfree: ", LIMINE_COLOR_LIGHT_GRAY);
    print_u64(video, heap_get_used(), LIMINE_COLOR_LIGHT_GREEN);
    video->printf(" B\n", LIMINE_COLOR_LIGHT_GRAY);
    video->printf("================================\n", LIMINE_COLOR_CYAN);
}

static void cmd_usb_debug(limine_video_driver* video, const char* arg) {
    int want = -1;
    if (str_cmp(arg, "on") == 0) want = 1;
    else if (str_cmp(arg, "off") == 0) want = 0;
    else if (arg[0] != '\0') {
        video->printf(" usage: usb-debug [on|off]\n", LIMINE_COLOR_LIGHT_RED);
        return;
    }

    if (want >= 0) {
        usb_log_set_level(want ? USB_LOG_TRACE : USB_LOG_EVENT);
        usb_msc_set_verbose(want);
    }

    video->printf(" USB verbose logging: ", LIMINE_COLOR_LIGHT_GRAY);
    if (usb_log_get_level() == USB_LOG_TRACE)
        video->printf("on\n", LIMINE_COLOR_LIGHT_GREEN);
    else
        video->printf("off\n", LIMINE_COLOR_AMBER);
}

static void get_list_disks(limine_video_driver* video){
    video->printf("\n=== Block Devices ===\n", LIMINE_COLOR_CYAN);
    int found = 0;
    for (int i = 0; i < MAX_BLOCK_DEVICES; i++) {
        struct block_device* dev = (struct block_device*)device_table[STORAGE_DEVICE][i];
        if (!dev) continue;

        found++;
        video->printf(" #", LIMINE_COLOR_LIGHT_GRAY);
        char dnum[3] = { '0' + (found / 10), '0' + (found % 10), '\0' };
        video->printf(dnum, LIMINE_COLOR_AMBER);
        video->printf(" ", 0);
        video->printf(dev->name, LIMINE_COLOR_LIGHT_GREEN);

        uint64_t size_mb = dev->sector_count * dev->sector_size / (1024 * 1024);
        video->printf(" - ", LIMINE_COLOR_WHITE);
        print_u64(video, size_mb, LIMINE_COLOR_AMBER);
        video->printf(" MB\n", LIMINE_COLOR_WHITE);
    }

    if (found == 0)
        video->printf(" No block devices found.\n", LIMINE_COLOR_LIGHT_RED);
    video->printf("=====================\n\n", LIMINE_COLOR_CYAN);
}

static void cmd_partitions(limine_video_driver* video) {
    video->printf("\n=== Partitions ===\n", LIMINE_COLOR_CYAN);

    int any_disk = 0;
    int any_partition = 0;

    for (int i = 0; i < MAX_BLOCK_DEVICES; i++) {
        struct block_device* dev = (struct block_device*)device_table[STORAGE_DEVICE][i];
        if (!dev) continue;

        partition_info_t infos[PART_MAX_PER_DISK];
        int n = 0;
        if (partition_scan(dev, infos, PART_MAX_PER_DISK, &n) != 0 || n == 0)
            continue;

        any_disk = 1;

        video->printf(" ", 0);
        video->printf(dev->name, LIMINE_COLOR_LIGHT_GREEN);
        video->printf(" (", LIMINE_COLOR_WHITE);
        video->printf(infos[0].is_gpt ? "GPT" : "MBR", LIMINE_COLOR_AMBER);
        video->printf("):\n", LIMINE_COLOR_WHITE);

        for (int j = 0; j < n; j++) {
            partition_info_t* p = &infos[j];
            any_partition = 1;

            video->printf("   ", 0);
            video->printf(p->name, LIMINE_COLOR_LIGHT_CYAN);
            video->printf("  LBA ", LIMINE_COLOR_LIGHT_GRAY);
            print_u64(video, p->start_lba, LIMINE_COLOR_AMBER);
            video->printf("..", LIMINE_COLOR_LIGHT_GRAY);
            print_u64(video, p->start_lba + p->sector_count - 1, LIMINE_COLOR_AMBER);

            uint64_t size_mb = p->sector_count * (dev->sector_size ? dev->sector_size : 512) / (1024 * 1024);
            video->printf("  (", LIMINE_COLOR_LIGHT_GRAY);
            print_u64(video, size_mb, LIMINE_COLOR_AMBER);
            video->printf(" MB)", LIMINE_COLOR_LIGHT_GRAY);

            if (p->is_gpt) {
                video->printf("  type-guid[0]=0x", LIMINE_COLOR_LIGHT_GRAY);
                print_u64(video, p->gpt_type_guid[0], LIMINE_COLOR_AMBER);
            } else {
                video->printf("  type=0x", LIMINE_COLOR_LIGHT_GRAY);
                print_u64(video, p->mbr_type, LIMINE_COLOR_AMBER);
                if (p->bootable) video->printf("  [bootable]", LIMINE_COLOR_LIGHT_GREEN);
            }
            video->printf("\n", 0);
        }
    }

    if (!any_disk)
        video->printf(" No partition tables found (all disks unpartitioned or empty).\n", LIMINE_COLOR_LIGHT_RED);
    else if (!any_partition)
        video->printf(" Partition tables found, but no partitions.\n", LIMINE_COLOR_AMBER);

    video->printf("=====================\n\n", LIMINE_COLOR_CYAN);
}


static void cmd_fs_mounts(limine_video_driver *video) {
    (void)video;
    int count = rootfs_mount_count();
    if (count == 0) {
        printf_color(0xFFFF5555, "Nothing is mounted.\n");
        return;
    }

    for (int i = 0; i < count; i++) {
        const rootfs_mount_t *m = rootfs_mount_at(i);
        if (!m) continue;

        if (m->fs.label[0])
            printf_color(0xFFDDDDDD, "  %-8s -> %-12s %s (label \"%s\")\n",
                         m->device, m->path, rootfs_type_name(m->fs.type), m->fs.label);
        else
            printf_color(0xFFDDDDDD, "  %-8s -> %-12s %s\n",
                         m->device, m->path, rootfs_type_name(m->fs.type));
    }
}

static void cmd_fs_mount(limine_video_driver *video, const char *args) {
    if (!args || args[0] == '\0') {
        cmd_fs_mounts(video);
        return;
    }

    char name[ROOTFS_DEVICE_NAME_MAX];
    char point[ROOTFS_MOUNT_PATH_MAX];

    size_t n = 0;
    while (args[n] && args[n] != ' ' && n < sizeof(name) - 1) { name[n] = args[n]; n++; }
    name[n] = '\0';

    const char *p = args + n;
    while (*p == ' ') p++;

    if (*p == '\0') {
        point[0] = '/'; point[1] = '\0';
    } else {
        size_t m = 0;
        while (p[m] && p[m] != ' ' && m < sizeof(point) - 1) { point[m] = p[m]; m++; }
        point[m] = '\0';
    }

    if (point[0] != '/') {
        printf_color(0xFFFF5555, "Mount point must be absolute, for example '/disk'.\n");
        return;
    }

    int fs_err = FS_OK;
    int r = rootfs_mount_device(name, point, &fs_err);

    switch (r) {
        case ROOTFS_OK:
            break;
        case ROOTFS_ERR_NODEV:
            printf_color(0xFFFF5555, "Device '%s' not found. Available devices: 'disks'.\n", name);
            return;
        case ROOTFS_ERR_BUSY:
            printf_color(0xFFFF5555, "'%s' or '%s' is already in use. Run 'umount' first.\n", name, point);
            return;
        case ROOTFS_ERR_NOROOT:
            printf_color(0xFFFF5555, "Mount '/' first, then mount disks into it.\n");
            return;
        case ROOTFS_ERR_NOENT:
            printf_color(0xFFFF5555, "Failed to create mount point '%s'.\n", point);
            return;
        case ROOTFS_ERR_NOSPACE:
            printf_color(0xFFFF5555, "Mount table is full.\n");
            return;
        case ROOTFS_ERR_FS:
            printf_color(0xFFFF5555, "Failed to mount '%s' (error code %d).\n", name, fs_err);
            return;
        default:
            printf_color(0xFFFF5555, "Usage: mount <disk_or_partition_name> [mount_point]\n");
            return;
    }

    const rootfs_mount_t *m = rootfs_mount_for_path(point);
    if (m && m->fs.label[0]) {
        printf_color(0xFF55FF55, "Mounted: '%s' (%s, label \"%s\") -> %s\n",
                     name, rootfs_type_name(m->fs.type), m->fs.label, point);
    } else if (m) {
        printf_color(0xFF55FF55, "Mounted: '%s' (%s) -> %s\n",
                     name, rootfs_type_name(m->fs.type), point);
    }
}

static void cmd_fs_umount(limine_video_driver *video, const char *arg) {
    (void)video;

    const char *point = (arg && arg[0]) ? arg : "/";

    char device[ROOTFS_DEVICE_NAME_MAX];
    const rootfs_mount_t *m = rootfs_mount_for_path(point);
    if (m) {
        strncpy(device, m->device, sizeof(device) - 1);
        device[sizeof(device) - 1] = '\0';
    } else {
        device[0] = '\0';
    }

    int r = rootfs_unmount_point(point);
    if (r == ROOTFS_ERR_NOENT) {
        printf_color(0xFFFF5555, "Nothing is mounted at '%s'.\n", point);
        return;
    }
    if (r == ROOTFS_ERR_BUSY) {
        printf_color(0xFFFF5555, "'%s' still has disks mounted inside it.\n", point);
        return;
    }
    if (r != ROOTFS_OK) {
        printf_color(0xFFFF5555, "umount: failed for '%s' (code %d)\n", point, r);
        return;
    }

    printf_color(0xFFAAAA00, "Disk '%s' unmounted from '%s'.\n", device, point);
}

static fs_t *shell_path(const char *arg, const char *usage, char *abs, size_t abs_cap,
                        char *rel, size_t rel_cap) {
    if (!rootfs_is_mounted()) {
        printf_color(0xFFFF5555, "Nothing is mounted. Use 'mount <disk>'.\n");
        return NULL;
    }
    if (usage && (!arg || arg[0] == '\0')) {
        printf_color(0xFFFF5555, "%s", usage);
        return NULL;
    }

    rootfs_normalize_path(arg, abs, abs_cap);

    fs_t *fs = rootfs_resolve(abs, rel, rel_cap);
    if (!fs) {
        printf_color(0xFFFF5555, "No filesystem mounted for '%s'.\n", abs);
        return NULL;
    }
    return fs;
}

static void cmd_fs_ls(limine_video_driver *video, const char *arg) {
    (void)video;
    char path[FS_MAX_PATH], rel[FS_MAX_PATH];
    fs_t *fs = shell_path(arg, NULL, path, sizeof(path), rel, sizeof(rel));
    if (!fs) return;

    fs_dir_t dir;
    int r = fs_opendir(fs, rel, &dir);
    if (r != FS_OK) { printf_color(0xFFFF5555, "ls: failed to open '%s' (code %d)\n", path, r); return; }

    fs_dirent_t ent;
    int count = 0;
    while (fs_readdir(&dir, &ent) == FS_OK) {
        if (ent.type == FS_ENTRY_DIR)
            printf_color(0xFF55FFFF, "  <DIR>       %s\n", ent.name);
        else
            printf_color(0xFFDDDDDD, "  %10llu  %s\n", (unsigned long long)ent.size, ent.name);
        count++;
    }
    fs_closedir(&dir);
    if (count == 0) printf_color(0xFF888888, "  (empty)\n");
}

static void cmd_fs_cd(limine_video_driver *video, const char *arg) {
    (void)video;
    char path[FS_MAX_PATH], rel[FS_MAX_PATH];
    fs_t *fs = shell_path(arg, "Usage: cd <path>\n", path, sizeof(path), rel, sizeof(rel));
    if (!fs) return;

    if (strcmp(rel, "/") != 0) {
        fs_dirent_t st;
        int r = fs_stat(fs, rel, &st);
        if (r != FS_OK) { printf_color(0xFFFF5555, "cd: '%s' does not exist (code %d)\n", path, r); return; }
        if (st.type != FS_ENTRY_DIR) { printf_color(0xFFFF5555, "cd: '%s' is not a directory\n", path); return; }
    }

    rootfs_set_cwd(path);
}

static void cmd_fs_pwd(limine_video_driver *video) {
    (void)video;
    if (!rootfs_is_mounted()) { printf_color(0xFFFF5555, "Nothing is mounted.\n"); return; }
    printf_color(0xFFDDDDDD, "%s\n", rootfs_cwd());
}

static void cmd_fs_mkdir(limine_video_driver *video, const char *arg) {
    (void)video;
    char path[FS_MAX_PATH], rel[FS_MAX_PATH];
    fs_t *fs = shell_path(arg, "Usage: mkdir <path>\n", path, sizeof(path), rel, sizeof(rel));
    if (!fs) return;

    int r = fs_mkdir(fs, rel);
    if (r == FS_OK) printf_color(0xFF55FF55, "Directory created: %s\n", path);
    else printf_color(0xFFFF5555, "mkdir: failed to create '%s' (code %d)\n", path, r);
}

static void cmd_fs_rmdir(limine_video_driver *video, const char *arg) {
    (void)video;
    char path[FS_MAX_PATH], rel[FS_MAX_PATH];
    fs_t *fs = shell_path(arg, "Usage: rmdir <path>\n", path, sizeof(path), rel, sizeof(rel));
    if (!fs) return;

    if (rootfs_mount_for_path(path) && strcmp(rel, "/") == 0) {
        printf_color(0xFFFF5555, "rmdir: '%s' is a mount point, run 'umount %s' first\n", path, path);
        return;
    }

    int r = fs_rmdir(fs, rel);
    if (r == FS_OK) printf_color(0xFF55FF55, "Directory removed: %s\n", path);
    else printf_color(0xFFFF5555, "rmdir: failed to remove '%s' (code %d)\n", path, r);
}

static void cmd_fs_rm(limine_video_driver *video, const char *arg) {
    (void)video;
    char path[FS_MAX_PATH], rel[FS_MAX_PATH];
    fs_t *fs = shell_path(arg, "Usage: rm <path>\n", path, sizeof(path), rel, sizeof(rel));
    if (!fs) return;

    int r = fs_unlink(fs, rel);
    if (r == FS_OK) printf_color(0xFF55FF55, "Removed: %s\n", path);
    else printf_color(0xFFFF5555, "rm: failed to remove '%s' (code %d)\n", path, r);
}

static void cmd_fs_cat(limine_video_driver *video, const char *arg) {
    (void)video;
    char path[FS_MAX_PATH], rel[FS_MAX_PATH];
    fs_t *fs = shell_path(arg, "Usage: cat <path>\n", path, sizeof(path), rel, sizeof(rel));
    if (!fs) return;

    fs_file_t f;
    int r = fs_open(fs, rel, false, false, &f);
    if (r != FS_OK) { printf_color(0xFFFF5555, "cat: failed to open '%s' (code %d)\n", path, r); return; }

    char buf[257];
    int64_t n;
    while ((n = fs_read(&f, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        printf_color(0xFFDDDDDD, "%s", buf);
    }
    printf_color(0xFFDDDDDD, "\n");
    fs_close(&f);
}

static void cmd_fs_touch(limine_video_driver *video, const char *arg) {
    (void)video;
    char path[FS_MAX_PATH], rel[FS_MAX_PATH];
    fs_t *fs = shell_path(arg, "Usage: touch <path>\n", path, sizeof(path), rel, sizeof(rel));
    if (!fs) return;

    fs_file_t f;
    int r = fs_open(fs, rel, true, false, &f);
    if (r == FS_OK) { fs_close(&f); printf_color(0xFF55FF55, "File created: %s\n", path); }
    else printf_color(0xFFFF5555, "touch: failed to create '%s' (code %d)\n", path, r);
}

static void cmd_fs_stat(limine_video_driver *video, const char *arg) {
    (void)video;
    char path[FS_MAX_PATH], rel[FS_MAX_PATH];
    fs_t *fs = shell_path(arg, "Usage: stat <path>\n", path, sizeof(path), rel, sizeof(rel));
    if (!fs) return;

    fs_dirent_t st;
    int r = fs_stat(fs, rel, &st);
    if (r != FS_OK) { printf_color(0xFFFF5555, "stat: '%s' not found (code %d)\n", path, r); return; }

    printf_color(0xFFDDDDDD, "  Name: %s\n", st.name);
    printf_color(0xFFDDDDDD, "  Path: %s\n", path);
    printf_color(0xFFDDDDDD, "  Type: %s\n", st.type == FS_ENTRY_DIR ? "directory" : "file");
    printf_color(0xFFDDDDDD, "  Size: %llu bytes\n", (unsigned long long)st.size);
}

static void cmd_mouse_status(limine_video_driver *video) {
    (void)video;

    int type = mouse_updater_active_type();
    const char *type_name = (type == PS2_MOUSE) ? "PS/2"
                          : (type == USB_MOUSE) ? "USB"
                          : "none";

    mouse_sample_t sample;
    mouse_updater_take(&sample);

    printf_color(0xFF55FFFF, "\n=== Mouse ===\n");
    printf_color(0xFFDDDDDD, "  Backend:  %s\n", type_name);
    printf_color(0xFFDDDDDD, "  Cursor:   x=%d y=%d\n",
                 (int)mouse_updater_x(), (int)mouse_updater_y());
    printf_color(0xFFDDDDDD, "  Buttons:  L=%d R=%d M=%d\n",
                 sample.btn_left ? 1 : 0, sample.btn_right ? 1 : 0, sample.btn_middle ? 1 : 0);
    printf_color(0xFFDDDDDD, "  Since last read: dx=%d dy=%d wheel=%d\n",
                 (int)sample.dx, (int)sample.dy, (int)sample.wheel);
    printf_color(0xFFDDDDDD, "  Polls:    %llu\n", (unsigned long long)mouse_updater_poll_count());
    printf_color(0xFFDDDDDD, "  Events:   %llu\n", (unsigned long long)mouse_updater_event_count());
    printf_color(0xFF55FFFF, "=============\n\n");
}

static void cmd_services(limine_video_driver *video) {
    (void)video;

    static const char *state_names[] = { "stopped", "starting", "running", "restart", "failed" };
    static const char *priority_names[] = { "root", "high", "normal", "low" };

    spin_lock(&services_lock);
    int count = services_count;
    spin_unlock(&services_lock);

    printf_color(0xFF55FFFF, "\n=== Services ===\n");

    for (int i = 0; i < count; i++) {
        service_t *svc = services_list[i];
        if (!svc) continue;

        const char *state = (svc->state < 5) ? state_names[svc->state] : "unknown";
        const char *prio = (svc->priority < SERVICE_PRIORITY_COUNT) ? priority_names[svc->priority] : "?";

        uint32_t color = (svc->state == SERVICE_STATE_RUNNING) ? 0xFF55FF55
                       : (svc->state == SERVICE_STATE_FAILED) ? 0xFFFF5555
                       : 0xFFAAAA00;

        printf_color(color, "  #%-2u %-18s %-8s prio=%-7s restarts=%u/%u\n",
                     svc->id, svc->name, state, prio,
                     (unsigned)svc->restart_count, (unsigned)svc->restart_limit);
    }

    printf_color(0xFF55FFFF, "================\n\n");
}

static void cmd_service_ctl(limine_video_driver *video, const char *args, int action) {
    (void)video;
    if (!args || args[0] == '\0') {
        printf_color(0xFFFF5555, "Usage: service-start|service-stop|service-restart <name>\n");
        return;
    }

    service_t *svc = get_service_by_name(args);
    if (!svc) {
        printf_color(0xFFFF5555, "Service '%s' not found. See 'services'.\n", args);
        return;
    }

    int r;
    if (action == 0) r = service_request_start(svc->id);
    else if (action == 1) r = service_request_stop(svc->id);
    else r = service_request_restart(svc->id);

    if (r != 0) printf_color(0xFFFF5555, "Request failed for '%s' (code %d).\n", args, r);
    else printf_color(0xFF55FF55, "Request accepted for '%s'.\n", args);
}

static void cmd_fastfetch(struct limine_video_driver* video) {
    (void)video;
    struct tsc_driver *tsc_SIK = get_self_driver(TIMER_DRIVER, TSC_TIMER);
    struct hpet_driver *hpet = get_self_driver(TIMER_DRIVER, HPET_TIMER);

    uint32_t c_logo = 0xFF55FFFF;
    uint32_t c_label = 0xFF55FF55;
    uint32_t c_white = 0xFFFFFFFF;
    uint32_t c_gray = 0xFF777777;

    uint64_t tsc_ms = tsc_SIK->get_tsc_uptime_ms();
    uint64_t tsc_s = tsc_ms / 1000;
    uint64_t t_m = (tsc_s % 3600) / 60;
    uint64_t t_s = tsc_s % 60;

    uint64_t hpet_ms = 0;
    if (hpet && hpet->is_hpet_available()) {
        hpet_ms = hpet->get_hpet_uptime_ms();
    }
    uint64_t hpet_s = hpet_ms / 1000;
    uint64_t h_m = (hpet_s % 3600) / 60;
    uint64_t h_s = hpet_s % 60;

    uint64_t p_free = pmm_free_page_count();
    uint64_t p_used = pmm_used_pages();
    uint64_t p_total = p_free + p_used;
    uint64_t pmm_perc = (p_total > 0) ? (p_used * 100) / p_total : 0;

    uint64_t h_total = heap_get_total();
    uint64_t h_used = heap_get_used();
    uint64_t h_perc = (h_total > 0) ? (h_used * 100) / h_total : 0;

    printf("\n");
    printf_color(c_logo, "  |\\        "); printf_color(c_label, "OS:      "); printf_color(c_white, "LuOS x86_64\n");
    printf_color(c_logo, "  | |       "); printf_color(c_label, "Kernel:  "); printf_color(c_white, KERNEL_VERSION " + " LUA_VERSION "\n");
    printf_color(c_logo, "  | |       "); printf_color(c_label, "Uptime:  ");
    printf_color(c_white, "%lum %lus (tsc)\n", t_m, t_s);
    printf_color(c_logo, "  | |  __   "); printf_color(c_label, "Uptime:  ");
    if (hpet_ms > 0) {
        printf_color(c_white, "%lum %lus ", h_m, h_s);
        printf_color(c_gray, "(hpet)\n");
    } else {
        printf_color(c_gray, "N/A (hpet)\n");
    }
    printf_color(c_logo, "  | | /  \\  "); printf_color(c_label, "Shell:   "); printf_color(c_white, "K-Shell v2.0\n");
    printf_color(c_logo, "  | |/ __/  "); printf_color(c_label, "Memory:  ");
    printf_color(c_white, "%lu MB / %lu MB\n", p_used / 256, p_total / 256);
    printf_color(c_logo, "  |___/     "); printf_color(c_label, "Heap:    ");
    printf_color(c_white, "%lu%% used (%lu MB)\n", h_perc, h_total / (1024 * 1024));
    printf_color(c_logo, "            "); printf_color(c_label, "Lua:     ");
    printf_color(g_lua ? c_white : 0xFFFF5555, g_lua ? LUA_VERSION " active\n" : "not loaded\n");
    printf_color(c_logo, "            "); printf_color(c_label, "Python:  ");
    if (g_mpy_ready) {
        printf_color(c_white, "MicroPython %s active\n", mp_luos_version_string());
    } else {
        printf_color(0xFFFF5555, "not loaded\n");
    }
    printf_color(c_label, "            Kernel Compiler:   " ); printf_color(c_white, KERNEL_COMPILER "\n");
    printf_color(c_label, "            Kernel Build Date: "); printf_color(c_white, KERNEL_COMPILATION_DATE "\n");

    printf("\n");
    printf_color(c_label, " [ RAM  ] ");
    printf("[");
    for (int i = 0; i < 20; i++) {
        if (i < (int)(pmm_perc / 5)) printf_color(0xFF55FF55, "#");
        else printf_color(0xFF444444, "-");
    }
    printf("] %lu%%\n", pmm_perc);

    printf_color(c_label, " [ HEAP ] ");
    printf("[");
    for (int i = 0; i < 20; i++) {
        if (i < (int)(h_perc / 5)) printf_color(0xFFE5C07B, "#");
        else printf_color(0xFF444444, "-");
    }
    printf("] %lu%%\n", h_perc);

    printf("\n ");
    uint32_t palette[] = {
        0xFFFF5555, 0xFF55FF55, 0xFFE5C07B, 0xFF5555FF,
        0xFFAA55FF, 0xFF55FFFF, 0xFFFFFFFF, 0xFF777777
    };
    for (int i = 0; i < 8; i++) printf_color(palette[i], "### ");
    printf("\n\n");
}


static volatile bool shell_ready = false;

static void shell_entry(void *arg) {
    (void)arg;

    service_t *svc = get_shell_service();

    g_lua = luos_lua_init();
    mp_luos_init();
    g_mpy_ready = 1;

    struct limine_video_driver* video = get_self_driver(LIMINE_VIDEO_DRIVER, 0);
    struct rtc_driver* rtc = get_self_driver(TIMER_DRIVER, RTC_TIMER);
    struct apic_driver* apic = get_self_driver(TIMER_DRIVER, APIC_TIMER);
    struct tsc_driver* tsc = get_self_driver(TIMER_DRIVER, TSC_TIMER);
    struct pit_driver* pit = get_self_driver(TIMER_DRIVER, PIT_TIMER);
    struct hpet_driver* hpet = get_self_driver(TIMER_DRIVER, HPET_TIMER);
    struct keyboard_driver* kbd = get_self_driver(KEYBOARD_DRIVER, VIRTUAL_KEYBOARD);
    struct mouse_driver* mouse = get_self_driver(MOUSE_DRIVER, VIRTUAL_MOUSE);

    g_bind_video = video;
    g_bind_rtc = rtc;
    g_bind_pit = pit;
    g_bind_tsc = tsc;
    mp_kernel_bind(video, rtc, pit, tsc);

    if (g_lua) {
        luos_register_kernel_lib(g_lua);
    }

    session_start_tsc = tsc ? tsc->get_tsc_ms() : 0;

    scheduler_sleep_ms(1000);
    video->clear(LIMINE_COLOR_BLACK);
    video->set_cursor_visible(true);

    draw_header(video);
    draw_system_info(video, rtc);
    video->printf(" > Initializing hardware... ", LIMINE_COLOR_WHITE);
    pit->sleep_pit_ms(300);
    video->printf("[OK]\n", LIMINE_COLOR_LIGHT_GREEN);

    video->printf(" > Initializing "LUA_VERSION"... ", LIMINE_COLOR_WHITE);
    video->printf(g_lua ? "[OK]\n" : "[FAIL]\n",
                  g_lua ? LIMINE_COLOR_LIGHT_GREEN : LIMINE_COLOR_LIGHT_RED);

    video->printf(" > Initializing MicroPython... ", LIMINE_COLOR_WHITE);
    video->printf(g_mpy_ready ? "[OK]\n" : "[FAIL]\n",
                  g_mpy_ready ? LIMINE_COLOR_LIGHT_GREEN : LIMINE_COLOR_LIGHT_RED);

    video->printf(" > Registering kernel.* API... ", LIMINE_COLOR_WHITE);
    video->printf(g_lua ? "[OK]\n" : "[SKIP]\n",
                  g_lua ? LIMINE_COLOR_LIGHT_GREEN : LIMINE_COLOR_DARK_GRAY);

    video->printf(" > HPET:     ", LIMINE_COLOR_LIGHT_GRAY);
    if (hpet && hpet->is_hpet_available()) {
        video->printf("OK  (", LIMINE_COLOR_LIGHT_GREEN);
        print_u64(video, hpet->get_hpet_frequency(), LIMINE_COLOR_AMBER);
        video->printf(" Hz)\n", LIMINE_COLOR_LIGHT_GRAY);
    } else {
        video->printf("not available\n", LIMINE_COLOR_DARK_GRAY);
    }

    int active_type = kbd->get_active_type();
    video->printf(" > Keyboard: ", LIMINE_COLOR_LIGHT_GRAY);
    if (active_type == PS2_KEYBOARD) video->printf("PS/2\n", LIMINE_COLOR_LIGHT_GREEN);
    else if (active_type == USB_KEYBOARD) video->printf("USB\n", LIMINE_COLOR_LIGHT_GREEN);
    else video->printf("None detected\n", LIMINE_COLOR_LIGHT_RED);

    video->printf(" > Mouse:    ", LIMINE_COLOR_LIGHT_GRAY);
    if (mouse) {
        int m_type = mouse->get_active_type();
        if (m_type == PS2_MOUSE) video->printf("PS/2\n", LIMINE_COLOR_LIGHT_GREEN);
        else if (m_type == USB_MOUSE) video->printf("USB\n", LIMINE_COLOR_LIGHT_GREEN);
        else video->printf("No mouse detected\n", LIMINE_COLOR_AMBER);
    } else {
        video->printf("driver not loaded\n", LIMINE_COLOR_LIGHT_RED);
    }

    video->printf(" > Lua:      ", LIMINE_COLOR_LIGHT_GRAY);
    video->printf(g_lua ? LUA_VERSION " ready (kernel.* API registered)\n"
                        : "failed to init\n",
                  g_lua ? LIMINE_COLOR_LIGHT_GREEN : LIMINE_COLOR_LIGHT_RED);

    char cmd_buffer[128];

    cmd_fastfetch(video);
    acpi_print_summary();

    shell_ready = true;

    while (!service_stop_requested(svc->id)) {
        kbd->input("luos # ", cmd_buffer, 128, LIMINE_COLOR_LIGHT_CYAN, video->printf);

        if (str_cmp(cmd_buffer, "help") == 0) {
            printf_color(LIMINE_COLOR_LIGHT_BLUE, "-- General --\n");
            printf_color(LIMINE_COLOR_AMBER, "%-46s %-40s\n", "help          - this help", "clear         - clear screen");
            printf_color(LIMINE_COLOR_AMBER, "%-46s %-40s\n", "ram           - memory/heap info", "time          - current time");
            printf_color(LIMINE_COLOR_AMBER, "%-46s %-40s\n", "cpu           - CPU information", "pci           - PCI devices");
            printf_color(LIMINE_COLOR_AMBER, "%-46s %-40s\n", "usb-list      - USB devices", "disks         - block devices");
            printf_color(LIMINE_COLOR_AMBER, "%-46s %-40s\n", "partitions    - partition tables", "usb-debug on|off - USB trace log");

            printf_color(LIMINE_COLOR_LIGHT_BLUE, "\n-- Filesystem --\n");
            printf_color(LIMINE_COLOR_LIGHT_GREEN, "%-46s %-40s\n", "mount NAME [POINT] - mount disk to POINT (default /)", "umount [POINT] - unmount POINT");
            printf_color(LIMINE_COLOR_LIGHT_GREEN, "%-46s %-40s\n", "mounts        - list mount points", "usb-rescan    - full USB re-enumeration");
            printf_color(LIMINE_COLOR_LIGHT_GREEN, "%-46s %-40s\n", "ls [PATH]     - list directory", "cd PATH       - change directory");
            printf_color(LIMINE_COLOR_LIGHT_GREEN, "%-46s %-40s\n", "pwd           - print cwd", "mkdir PATH    - create directory");
            printf_color(LIMINE_COLOR_LIGHT_GREEN, "%-46s %-40s\n", "rmdir PATH    - remove empty dir", "rm PATH       - delete file");
            printf_color(LIMINE_COLOR_LIGHT_GREEN, "%-46s %-40s\n", "cat PATH      - print contents", "touch PATH    - create empty file");
            printf_color(LIMINE_COLOR_LIGHT_GREEN, "%-46s %-40s\n", "stat PATH     - name/type/size", "partitions    - disk partition tables");

            printf_color(LIMINE_COLOR_LIGHT_BLUE, "\n-- Hardware / Input --\n");
            printf_color(LIMINE_COLOR_AMBER, "%-46s %-40s\n", "usb           - USB controllers", "usb-ports     - root hub ports");
            printf_color(LIMINE_COLOR_AMBER, "%-46s %-40s\n", "kbd-test      - keyboard test", "mouse-test    - mouse test (5s)");
            printf_color(LIMINE_COLOR_AMBER, "%-46s %-40s\n", "mouse         - live cursor/buttons from mouse service", "");
            printf_color(LIMINE_COLOR_AMBER, "%-46s %-40s\n", "hid-test      - kbd+mouse test (15s)", "hpet          - HPET info/self-test");
            printf_color(LIMINE_COLOR_AMBER, "%-46s %-40s\n", "scheduler-test [cores] [tasks] [iters] - scheduler/multitask test", "guess-game    - guess the number");

            printf_color(LIMINE_COLOR_LIGHT_BLUE, "\n-- Lua / Python --\n");
            printf_color(LIMINE_COLOR_CYAN, "%-46s %-40s\n", "lua-test      - Lua test suite", "lua-kernel-test - kernel.* bindings");
            printf_color(LIMINE_COLOR_CYAN, "%-46s %-40s\n", "lua           - Lua REPL", "lua-run CODE  - run one line of Lua");
            printf_color(LIMINE_COLOR_LIGHT_MAGENTA, "%-46s %-40s\n", "python (py)   - MicroPython REPL", "python-run CODE - run one line");
            printf_color(LIMINE_COLOR_LIGHT_MAGENTA, "%-46s %-40s\n", "python-test   - Python test suite", "python-kernel-test - kernel.* bind.");
            printf_color(LIMINE_COLOR_CYAN, "%-46s %-40s\n", "lua-file F    - run a .lua file from /", "python-file F - run a .py file from /");
            printf_color(LIMINE_COLOR_CYAN, "%-46s %-40s\n", "run FILE      - run .lua or .py by extension", "lua-io-test   - io.read() keyboard test");

            printf_color(LIMINE_COLOR_LIGHT_BLUE, "\n-- Timers / Misc --\n");
            printf_color(LIMINE_COLOR_AMBER, "%-46s %-40s\n", "sleep N       - sleep ms (APIC)", "sleepRTC N    - sleep sec (RTC)");

            if (tsc)
                printf_color(LIMINE_COLOR_AMBER, "%-46s", "sleepTSC N    - sleep ms (TSC)");

            if (pit)
                printf_color(LIMINE_COLOR_AMBER, "%-46s", " sleepPIT N    - sleep ms (PIT)");

            if (tsc || pit)
                printf_color(LIMINE_COLOR_AMBER, "\n");

            if (hpet && hpet->is_hpet_available())
                printf_color(LIMINE_COLOR_AMBER, "%-46s\n", "sleepHPET N   - sleep ms (HPET)");

            printf_color(LIMINE_COLOR_AMBER, "%-46s %-40s\n", "allocate N    - leak test", "math-test     - math diagnostics");
            printf_color(LIMINE_COLOR_AMBER, "%-46s %-40s\n", "libc-test     - libc diagnostics", "fastfetch     - system info");
            printf_color(LIMINE_COLOR_AMBER, "%-46s %-40s\n", "repeat N TEXT - repeat TEXT N times", "pasha         - special message");

            printf_color(LIMINE_COLOR_LIGHT_BLUE, "\n-- Games --\n");
            printf_color(LIMINE_COLOR_AMBER, "%-46s %-40s\n", "snake-game    - snake", "bird-game     - flappy bird");
            printf_color(LIMINE_COLOR_AMBER, "%-46s %-40s\n", "race-game     - race game", "rps-game      - rock-paper-scissors");
            printf_color(LIMINE_COLOR_AMBER, "%-46s %-40s\n", "pong-game     - ping pong", "arco-game     - arkanoid");
            printf_color(LIMINE_COLOR_AMBER, "%-46s %-40s\n", "tetris-game   - tetris", "2048-game     - 2048");
            printf_color(LIMINE_COLOR_AMBER, "%-46s %-40s\n", "3d-cube [W H] - spinning wireframe shapes (W,H = pixels)", "reboot        - reboot system");

            printf_color(LIMINE_COLOR_LIGHT_BLUE, "\n-- Power / Debug --\n");

            printf_color(LIMINE_COLOR_AMBER, "%-46s %-40s\n", "poweroff      - shutdown system", "debug         - debug menu");

            printf_color(LIMINE_COLOR_LIGHT_BLUE, "\n-- Services --\n");
            printf_color(LIMINE_COLOR_CYAN, "%-46s %-40s\n", "services      - service table", "service-start NAME - start service");
            printf_color(LIMINE_COLOR_CYAN, "%-46s %-40s\n", "service-stop NAME  - stop service", "service-restart NAME - restart service");
        }
        else if (str_cmp(cmd_buffer, "clear") == 0) {
            video->clear(LIMINE_COLOR_BLACK);
        }
        else if (str_cmp(cmd_buffer, "ram") == 0) {
            cmd_ram(video);
        }
        else if (str_cmp(cmd_buffer, "time") == 0) {
            struct system_time* t = rtc->get_rtc_time();
            char time_str[9];
            time_str[0] = '0' + (t->hours / 10); time_str[1] = '0' + (t->hours % 10);
            time_str[2] = ':';
            time_str[3] = '0' + (t->minutes / 10); time_str[4] = '0' + (t->minutes % 10);
            time_str[5] = ':';
            time_str[6] = '0' + (t->seconds / 10); time_str[7] = '0' + (t->seconds % 10);
            time_str[8] = '\0';
            video->printf("Time: ", LIMINE_COLOR_WHITE);
            video->printf(time_str, LIMINE_COLOR_AMBER);
            video->printf("\n", 0);
        }
        else if (str_cmp(cmd_buffer, "cpu") == 0) { show_cpu_info(video); }
        else if (str_cmp(cmd_buffer, "pci") == 0) { show_pci_info(video, apic); }
        else if (str_cmp(cmd_buffer, "usb") == 0) { show_usb_controllers(video); }
        else if (str_cmp(cmd_buffer, "usb-ports") == 0) { cmd_usb_ports(video); }
        else if (str_cmp(cmd_buffer, "usb-list") == 0) { cmd_usb_list(video); }
        else if (str_cmp(cmd_buffer, "usb-debug") == 0) { cmd_usb_debug(video, ""); }
        else if (str_starts_with(cmd_buffer, "usb-debug ")) { cmd_usb_debug(video, cmd_buffer + 10); }
        else if (str_cmp(cmd_buffer, "disks") == 0) { get_list_disks(video); }
        else if (str_cmp(cmd_buffer, "partitions") == 0) { cmd_partitions(video); }
        else if (str_starts_with(cmd_buffer, "mount ")) { cmd_fs_mount(video, cmd_buffer + 6); }
        else if (str_cmp(cmd_buffer, "mount") == 0) { cmd_fs_mount(video, ""); }
        else if (str_cmp(cmd_buffer, "mounts") == 0) { cmd_fs_mounts(video); }
        else if (str_starts_with(cmd_buffer, "umount ")) { cmd_fs_umount(video, cmd_buffer + 7); }
        else if (str_cmp(cmd_buffer, "umount") == 0) { cmd_fs_umount(video, ""); }
        else if (str_cmp(cmd_buffer, "services") == 0) { cmd_services(video); }
        else if (str_starts_with(cmd_buffer, "service-start ")) { cmd_service_ctl(video, cmd_buffer + 14, 0); }
        else if (str_starts_with(cmd_buffer, "service-stop ")) { cmd_service_ctl(video, cmd_buffer + 13, 1); }
        else if (str_starts_with(cmd_buffer, "service-restart ")) { cmd_service_ctl(video, cmd_buffer + 16, 2); }
        else if (str_cmp(cmd_buffer, "usb-rescan") == 0) {
            usb_hotplug_request_rescan();
            video->printf("Full USB re-enumeration requested (drops every device first).\n", LIMINE_COLOR_AMBER);
        }
        else if (str_cmp(cmd_buffer, "ls") == 0) { cmd_fs_ls(video, ""); }
        else if (str_starts_with(cmd_buffer, "ls ")) { cmd_fs_ls(video, cmd_buffer + 3); }
        else if (str_starts_with(cmd_buffer, "cd ")) { cmd_fs_cd(video, cmd_buffer + 3); }
        else if (str_cmp(cmd_buffer, "cd") == 0) { cmd_fs_cd(video, "/"); }
        else if (str_cmp(cmd_buffer, "pwd") == 0) { cmd_fs_pwd(video); }
        else if (str_starts_with(cmd_buffer, "mkdir ")) { cmd_fs_mkdir(video, cmd_buffer + 6); }
        else if (str_starts_with(cmd_buffer, "rmdir ")) { cmd_fs_rmdir(video, cmd_buffer + 6); }
        else if (str_starts_with(cmd_buffer, "rm ")) { cmd_fs_rm(video, cmd_buffer + 3); }
        else if (str_starts_with(cmd_buffer, "cat ")) { cmd_fs_cat(video, cmd_buffer + 4); }
        else if (str_starts_with(cmd_buffer, "touch ")) { cmd_fs_touch(video, cmd_buffer + 6); }
        else if (str_starts_with(cmd_buffer, "stat ")) { cmd_fs_stat(video, cmd_buffer + 5); }
        else if (str_cmp(cmd_buffer, "kbd-test") == 0) { cmd_kbd_test(video, kbd); }
        else if (str_cmp(cmd_buffer, "mouse") == 0) { cmd_mouse_status(video); }
        else if (str_cmp(cmd_buffer, "mouse-test") == 0) { cmd_mouse_test(video, mouse, tsc); }
        else if (str_cmp(cmd_buffer, "hid-test") == 0) { cmd_hid_test(video, kbd, mouse, tsc); }
        else if (str_starts_with(cmd_buffer, "scheduler-test ")) { cmd_scheduler_test(video, cmd_buffer + 15); }
        else if (str_cmp(cmd_buffer, "scheduler-test") == 0) { cmd_scheduler_test(video, ""); }
        else if (str_cmp(cmd_buffer, "hpet") == 0) { cmd_hpet(video); }

        else if (str_cmp(cmd_buffer, "lua-test") == 0) { cmd_lua_test(video); }
        else if (str_cmp(cmd_buffer, "lua-kernel-test") == 0) { cmd_lua_kernel_bindings_test(video); }
        else if (str_cmp(cmd_buffer, "lua") == 0) { cmd_lua_repl(video, kbd); }
        else if (str_starts_with(cmd_buffer, "lua-run ")) {
            if (g_lua) {
                luos_lua_dostring(g_lua, cmd_buffer + 8);
            } else {
                video->printf("Lua not initialized!\n", LIMINE_COLOR_LIGHT_RED);
            }
        }
        else if (str_starts_with(cmd_buffer, "lua-file ")) { cmd_lua_file(video, cmd_buffer + 9); }
        else if (str_starts_with(cmd_buffer, "python-file ")) { cmd_python_file(video, cmd_buffer + 12); }
        else if (str_starts_with(cmd_buffer, "py-file ")) { cmd_python_file(video, cmd_buffer + 8); }
        else if (str_starts_with(cmd_buffer, "run ")) { cmd_run_script(video, cmd_buffer + 4); }
        else if (str_cmp(cmd_buffer, "lua-io-test") == 0) { cmd_lua_io_test(video); }

        else if (str_cmp(cmd_buffer, "python") == 0 || str_cmp(cmd_buffer, "py") == 0) {
            cmd_python_repl(video, kbd);
        }
        else if (str_cmp(cmd_buffer, "python-test") == 0 || str_cmp(cmd_buffer, "py-test") == 0) {
            cmd_python_test(video);
        }
        else if (str_cmp(cmd_buffer, "python-kernel-test") == 0 || str_cmp(cmd_buffer, "py-kernel-test") == 0) {
            cmd_python_kernel_bindings_test(video);
        }
        else if (str_starts_with(cmd_buffer, "python-run ")) {
            if (g_mpy_ready) {
                mp_luos_exec_str(cmd_buffer + 11);
            } else {
                video->printf("MicroPython not initialized!\n", LIMINE_COLOR_LIGHT_RED);
            }
        }
        else if (str_starts_with(cmd_buffer, "py-run ")) {
            if (g_mpy_ready) {
                mp_luos_exec_str(cmd_buffer + 7);
            } else {
                video->printf("MicroPython not initialized!\n", LIMINE_COLOR_LIGHT_RED);
            }
        }

        else if (str_cmp(cmd_buffer, "fastfetch") == 0 ||
                 str_cmp(cmd_buffer, "ff") == 0 ||
                 str_cmp(cmd_buffer, "neofetch") == 0) {
            cmd_fastfetch(video);
        }
        else if (str_cmp(cmd_buffer, "math-test") == 0) { cmd_math_test(video); }
        else if (str_cmp(cmd_buffer, "libc-test") == 0) { cmd_libc_test(video); }
        else if (str_starts_with(cmd_buffer, "sleep ")) {
            unsigned int ms = parse_number(cmd_buffer + 6);
            if (ms > 0) {
                video->printf("Sleeping (APIC)...", LIMINE_COLOR_WHITE);
                apic->sleep_apic_ms(ms);
                video->printf(" done!\n", LIMINE_COLOR_LIGHT_GREEN);
            }
        }
        else if (str_starts_with(cmd_buffer, "sleepTSC ")) {
            unsigned int ms = parse_number(cmd_buffer + 9);
            if (ms > 0 && tsc) { video->printf("Sleeping (TSC)...", LIMINE_COLOR_WHITE); tsc->sleep_tsc_ms(ms); video->printf(" done!\n", LIMINE_COLOR_LIGHT_GREEN); }
        }
        else if (str_starts_with(cmd_buffer, "sleepPIT ")) {
            unsigned int ms = parse_number(cmd_buffer + 9);
            if (ms > 0 && pit) { video->printf("Sleeping (PIT)...", LIMINE_COLOR_WHITE); pit->sleep_pit_ms(ms); video->printf(" done!\n", LIMINE_COLOR_LIGHT_GREEN); }
        }
        else if (str_starts_with(cmd_buffer, "sleepHPET ")) {
            unsigned int ms = parse_number(cmd_buffer + 10);
            if (!hpet || !hpet->is_hpet_available()) { video->printf("HPET not available on this system.\n", LIMINE_COLOR_LIGHT_RED); }
            else if (ms == 0) { video->printf("Usage: sleepHPET <milliseconds>\n", LIMINE_COLOR_LIGHT_RED); }
            else { video->printf("Sleeping (HPET)...", LIMINE_COLOR_WHITE); hpet->sleep_hpet_ms(ms); video->printf(" done!\n", LIMINE_COLOR_LIGHT_GREEN); }
        }
        else if (str_starts_with(cmd_buffer, "allocate ")) {
            unsigned int target_mb = parse_number(cmd_buffer + 9);
            if (target_mb == 0) { video->printf("Usage: allocate <megabytes>\n", LIMINE_COLOR_LIGHT_RED); }
            else {
                video->printf("Allocating ", LIMINE_COLOR_WHITE);
                print_u64(video, (uint64_t)target_mb, LIMINE_COLOR_AMBER);
                video->printf(" MB... \n", LIMINE_COLOR_WHITE);
                unsigned int allocated_mb = 0;
                for (unsigned int i = 0; i < target_mb; i++) {
                    void* chunk = kmalloc(1024 * 1024);
                    if (!chunk) {
                        video->printf("\n[ERROR] Out of memory! Stopped at ", LIMINE_COLOR_LIGHT_RED);
                        print_u64(video, (uint64_t)allocated_mb, LIMINE_COLOR_AMBER);
                        video->printf(" MB.\n", LIMINE_COLOR_LIGHT_RED);
                        break;
                    }
                    allocated_mb++;
                    if (allocated_mb % 10 == 0 || allocated_mb == target_mb)
                        video->printf(".", LIMINE_COLOR_LIGHT_GREEN);
                }
                if (allocated_mb == target_mb) {
                    video->printf("\n[SUCCESS] Allocated ", LIMINE_COLOR_LIGHT_GREEN);
                    print_u64(video, (uint64_t)allocated_mb, LIMINE_COLOR_AMBER);
                    video->printf(" MB successfully.\n", LIMINE_COLOR_LIGHT_GREEN);
                }
            }
        }
        else if (str_starts_with(cmd_buffer, "sleepRTC ")) {
            unsigned int seconds = parse_number(cmd_buffer + 9);
            if (seconds > 0) {
                video->printf("Sleeping (RTC)...", LIMINE_COLOR_WHITE);
                struct system_time* start_time = rtc->get_rtc_time();
                unsigned int start_s = start_time->hours * 3600 + start_time->minutes * 60 + start_time->seconds;
                while (1) {
                    struct system_time* cur = rtc->get_rtc_time();
                    unsigned int cur_s = cur->hours * 3600 + cur->minutes * 60 + cur->seconds;
                    unsigned int diff = (cur_s >= start_s) ? (cur_s - start_s) : (86400 - start_s + cur_s);
                    if (diff >= seconds) break;
                    pit->sleep_pit_ms(100);
                }
                video->printf(" done!\n", LIMINE_COLOR_LIGHT_GREEN);
            }
        }
        else if (str_starts_with(cmd_buffer, "repeat ")) { handle_repeat_command(video, cmd_buffer); }
        else if (str_cmp(cmd_buffer, "pasha") == 0) { video->printf("PASHOK IS THE BEST!\n", LIMINE_COLOR_LIGHT_MAGENTA); }
        else if (str_cmp(cmd_buffer, "guess-game") == 0) { guess_number_game(video, rtc, kbd, apic); }
        else if (str_cmp(cmd_buffer, "snake-game") == 0) { snake_game(video, kbd, tsc, rtc); video->clear(LIMINE_COLOR_BLACK); draw_header(video); draw_system_info(video, rtc); }
        else if (str_cmp(cmd_buffer, "rps-game") == 0) { rps_game(video, kbd); video->clear(LIMINE_COLOR_BLACK); draw_header(video); draw_system_info(video, rtc); }
        else if (str_cmp(cmd_buffer, "bird-game") == 0) { flappy_bird_game(video, kbd, tsc); video->clear(LIMINE_COLOR_BLACK); draw_header(video); draw_system_info(video, rtc); }
        else if (str_cmp(cmd_buffer, "race-game") == 0) { racing_game(video, kbd, tsc); video->clear(LIMINE_COLOR_BLACK); draw_header(video); draw_system_info(video, rtc); }
        else if (str_cmp(cmd_buffer, "pong-game") == 0) { pong_game(video, kbd, tsc); video->clear(LIMINE_COLOR_BLACK); draw_header(video); draw_system_info(video, rtc); }
        else if (str_cmp(cmd_buffer, "arco-game") == 0) { breakout_game(video, kbd, tsc); video->clear(LIMINE_COLOR_BLACK); draw_header(video); draw_system_info(video, rtc); }
        else if (str_cmp(cmd_buffer, "tetris-game") == 0) { tetris_game(video, kbd, tsc); video->clear(LIMINE_COLOR_BLACK); draw_header(video); draw_system_info(video, rtc); }
        else if (str_cmp(cmd_buffer, "2048-game") == 0) { game2048_game(video, kbd, tsc); video->clear(LIMINE_COLOR_BLACK); draw_header(video); draw_system_info(video, rtc); }
        else if (str_cmp(cmd_buffer, "3d-cube") == 0) { cube3d_game(video, kbd, tsc, 0, 0); video->clear(LIMINE_COLOR_BLACK); draw_header(video); draw_system_info(video, rtc); }
        else if (str_starts_with(cmd_buffer, "3d-cube ")) {
            const char* args = cmd_buffer + 8;
            while (*args == ' ') args++;
            int cube_w = (int)parse_number(args);
            const char* p = args;
            while (*p >= '0' && *p <= '9') p++;
            while (*p == ' ') p++;
            int cube_h = (int)parse_number(p);
            cube3d_game(video, kbd, tsc, cube_w, cube_h);
            video->clear(LIMINE_COLOR_BLACK); draw_header(video); draw_system_info(video, rtc);
        }
        else if (str_cmp(cmd_buffer, "reboot") == 0) {
            video->printf("Rebooting...\n", LIMINE_COLOR_AMBER);
            acpi_reboot();
        }
        else if (str_cmp(cmd_buffer, "shutdown") == 0 || str_cmp(cmd_buffer, "poweroff") == 0) {
            video->printf("Shutting down...\n", LIMINE_COLOR_AMBER);
            if (!acpi_shutdown()) {
                video->printf("ACPI shutdown unavailable on this platform.\n", LIMINE_COLOR_LIGHT_RED);
            }
        }
        else if (str_cmp(cmd_buffer, "panic") == 0 ||
                 str_starts_with(cmd_buffer, "panic ")) {
            const char *msg = str_starts_with(cmd_buffer, "panic ")
                              ? cmd_buffer + 6
                              : "Manual panic triggered from shell";
            static cpu_regs_t shell_regs = {0};
            struct panic_info info = { PANIC_CODE_GENERAL, msg, &shell_regs };
            panic(&info);
        }
        else if (cmd_buffer[0] != '\0') {
            video->printf("Unknown command. Type 'help' for list.\n", LIMINE_COLOR_LIGHT_RED);
        }
    }
}

bool shell_is_ready(void) {
    return shell_ready;
}

service_t *get_shell_service(void) {
    static service_t shell_service = {
        .name = "shell",
        .entry = shell_entry,
        .arg = NULL,
        .update = NULL,
        .priority = SERVICE_NORMAL_PRIORITY,
        .state = SERVICE_STATE_STOPPED,
        .dependency_count = 0,
        .restart_limit = SERVICE_RESTART_LIMIT_DEFAULT,
        .restart_count = 0,
        .task = NULL
    };

    return &shell_service;
}
