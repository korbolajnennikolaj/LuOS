// modkernel.c — the "kernel" module for MicroPython on LuOS.
//
// This is a direct counterpart of the kernel.* table from the Lua
// bindings (see kernel_lib[] and lua_kernel_* in src/kernel/kernel.c)
// — the same capabilities, the same set of functions, just from the
// Python side: import kernel; kernel.version() etc.
//
// Driver pointers (video/rtc/pit/tsc) are passed in here once from
// kernel.c via mp_kernel_bind(), exactly the way it's set up for Lua
// via the static g_bind_* in kernel.c.

#include <string.h>
#include "py/obj.h"
#include "py/runtime.h"
#include "py/objstr.h"
#include "py/mperrno.h"

#include "mp_kernel.h"

#include "drivers/Video/limine_video_driver.h"
#include "drivers/Timer/rtc_driver.h"
#include "drivers/Timer/pit_driver.h"
#include "drivers/Timer/tsc_driver.h"
#include <cpuid.h>
#include "components/Memory/pmm.h"
#include "components/Memory/heap.h"
#include "kernel/kernel_info.h"

// ----------------------------------------------------------------------
// Driver binding (called once from kernel.c at startup)
// ----------------------------------------------------------------------

static struct limine_video_driver *g_mpy_video = (void *)0;
static struct rtc_driver *g_mpy_rtc = (void *)0;
static struct pit_driver *g_mpy_pit = (void *)0;
static struct tsc_driver *g_mpy_tsc = (void *)0;

void mp_kernel_bind(struct limine_video_driver *video, struct rtc_driver *rtc,
                     struct pit_driver *pit, struct tsc_driver *tsc) {
    g_mpy_video = video;
    g_mpy_rtc = rtc;
    g_mpy_pit = pit;
    g_mpy_tsc = tsc;
}

// Used by other built-in modules (random, time) as a common "ticks"
// source — TSC, if the kernel has already initialized it.
uint64_t mp_kernel_uptime_ms(void) {
    return g_mpy_tsc ? g_mpy_tsc->get_tsc_uptime_ms() : 0;
}

// ----------------------------------------------------------------------
// [Output] kernel.print / kernel.print_color / kernel.clear
// ----------------------------------------------------------------------

static mp_obj_t kernel_print(mp_obj_t text_obj) {
    const char *text = mp_obj_str_get_str(text_obj);
    if (g_mpy_video) {
        g_mpy_video->printf(text, 0xFFFFFFFFu);
        g_mpy_video->printf("\n", 0xFFFFFFFFu);
    } else {
        mp_printf(&mp_plat_print, "%s\n", text);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(kernel_print_obj, kernel_print);

static mp_obj_t kernel_print_color(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    const char *text = mp_obj_str_get_str(args[0]);
    mp_int_t r = mp_obj_get_int(args[1]);
    mp_int_t g = mp_obj_get_int(args[2]);
    mp_int_t b = mp_obj_get_int(args[3]);

    uint32_t color = 0xFF000000u
                    | ((uint32_t)(r & 0xFF) << 16)
                    | ((uint32_t)(g & 0xFF) << 8)
                    | ((uint32_t)(b & 0xFF));

    if (g_mpy_video) {
        g_mpy_video->printf(text, color);
        g_mpy_video->printf("\n", color);
    } else {
        mp_printf(&mp_plat_print, "%s\n", text);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(kernel_print_color_obj, 4, 4, kernel_print_color);

static mp_obj_t kernel_clear(void) {
    if (g_mpy_video) {
        g_mpy_video->clear(0xFF000000u);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(kernel_clear_obj, kernel_clear);

// ----------------------------------------------------------------------
// [System] kernel.version / get_cpu_vendor / has_apic / has_sse / has_avx
// ----------------------------------------------------------------------

static mp_obj_t kernel_version(void) {
    char buf[128];
    // %s isn't always available in every build of the kernel runtime's
    // mp_printf-like functions, so we build the string by hand via kernel snprintf.
    snprintf(buf, sizeof(buf), "LuOS Kernel %s (MicroPython %s)",
             KERNEL_VERSION, MICROPY_VERSION_STRING);
    return mp_obj_new_str(buf, strlen(buf));
}
static MP_DEFINE_CONST_FUN_OBJ_0(kernel_version_obj, kernel_version);

static mp_obj_t kernel_get_cpu_vendor(void) {
    char vendor[13];
    cpuid_get_vendor(vendor);
    vendor[12] = '\0';
    return mp_obj_new_str(vendor, strlen(vendor));
}
static MP_DEFINE_CONST_FUN_OBJ_0(kernel_get_cpu_vendor_obj, kernel_get_cpu_vendor);

static mp_obj_t kernel_has_apic(void) {
    return mp_obj_new_bool(cpuid_has_apic());
}
static MP_DEFINE_CONST_FUN_OBJ_0(kernel_has_apic_obj, kernel_has_apic);

static mp_obj_t kernel_has_sse(void) {
    struct cpuid_result info = cpuid_get_processor_info();
    return mp_obj_new_bool((info.edx & CPUID_FEATURE_SSE) != 0);
}
static MP_DEFINE_CONST_FUN_OBJ_0(kernel_has_sse_obj, kernel_has_sse);

static mp_obj_t kernel_has_avx(void) {
    struct cpuid_result info = cpuid_get_processor_info();
    return mp_obj_new_bool((info.ecx & CPUID_FEATURE_AVX) != 0);
}
static MP_DEFINE_CONST_FUN_OBJ_0(kernel_has_avx_obj, kernel_has_avx);

// ----------------------------------------------------------------------
// [Time] kernel.get_time / get_tsc_ms / sleep_ms
// ----------------------------------------------------------------------

static mp_obj_t kernel_get_time(void) {
    if (!g_mpy_rtc) {
        mp_raise_OSError(MP_EIO); // RTC driver not available
    }
    struct system_time *t = g_mpy_rtc->get_rtc_time();

    mp_obj_t dict = mp_obj_new_dict(3);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_hours), mp_obj_new_int(t->hours));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_minutes), mp_obj_new_int(t->minutes));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_seconds), mp_obj_new_int(t->seconds));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(kernel_get_time_obj, kernel_get_time);

static mp_obj_t kernel_get_tsc_ms(void) {
    if (!g_mpy_tsc) {
        return mp_obj_new_int(0);
    }
    uint64_t ms = g_mpy_tsc->get_tsc_uptime_ms();
    return mp_obj_new_int_from_uint(ms);
}
static MP_DEFINE_CONST_FUN_OBJ_0(kernel_get_tsc_ms_obj, kernel_get_tsc_ms);

static mp_obj_t kernel_sleep_ms(mp_obj_t ms_obj) {
    mp_int_t ms = mp_obj_get_int(ms_obj);
    if (ms <= 0) return mp_const_none;

    if (g_mpy_tsc) {
        g_mpy_tsc->sleep_tsc_ms((unsigned int)ms);
    } else if (g_mpy_pit) {
        g_mpy_pit->sleep_pit_ms((unsigned int)ms);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(kernel_sleep_ms_obj, kernel_sleep_ms);

// ----------------------------------------------------------------------
// [Memory] kernel.get_ram_mb / get_heap_stats / kmalloc_test
// ----------------------------------------------------------------------

static mp_obj_t kernel_get_ram_mb(void) {
    uint64_t free_pages = pmm_free_page_count();
    uint64_t used_pages = pmm_used_pages();
    uint64_t total_pages = free_pages + used_pages;

    mp_obj_t dict = mp_obj_new_dict(3);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_total_mb), mp_obj_new_int_from_uint(total_pages / 256));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_free_mb), mp_obj_new_int_from_uint(free_pages / 256));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_used_mb), mp_obj_new_int_from_uint(used_pages / 256));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(kernel_get_ram_mb_obj, kernel_get_ram_mb);

static mp_obj_t kernel_get_heap_stats(void) {
    uint64_t total = heap_get_total();
    uint64_t used = heap_get_used();
    uint64_t free = (total > used) ? (total - used) : 0;

    mp_obj_t dict = mp_obj_new_dict(3);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_total), mp_obj_new_int_from_uint(total));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_used), mp_obj_new_int_from_uint(used));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_free), mp_obj_new_int_from_uint(free));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(kernel_get_heap_stats_obj, kernel_get_heap_stats);

static mp_obj_t kernel_kmalloc_test(mp_obj_t size_obj) {
    mp_int_t size = mp_obj_get_int(size_obj);
    if (size <= 0 || size > 64 * 1024 * 1024) {
        return mp_obj_new_int(0);
    }

    void *ptr = kmalloc((size_t)size);
    if (!ptr) {
        return mp_obj_new_int(0);
    }
    kfree(ptr);
    return mp_obj_new_int_from_uint((mp_uint_t)(uintptr_t)ptr);
}
static MP_DEFINE_CONST_FUN_OBJ_1(kernel_kmalloc_test_obj, kernel_kmalloc_test);

// ----------------------------------------------------------------------
// kernel.help() — brief kernel.* reference right in the REPL
// ----------------------------------------------------------------------

static mp_obj_t kernel_help(void) {
    mp_printf(&mp_plat_print,
        "\n================================================\n"
        "   kernel.*  -  LuOS Python API  (" KERNEL_VERSION ")\n"
        "================================================\n"
        "\n[Output]\n"
        "  kernel.print(text)               print text\n"
        "  kernel.print_color(text,r,g,b)   print with RGB(0-255) color\n"
        "  kernel.clear()                   clear screen to black\n"
        "  kernel.help()                    show this help\n"
        "\n[System]\n"
        "  kernel.version()                 kernel version string\n"
        "  kernel.get_cpu_vendor()          CPU vendor string via CPUID\n"
        "  kernel.has_apic()                bool - APIC present\n"
        "  kernel.has_sse()                 bool - SSE present\n"
        "  kernel.has_avx()                 bool - AVX present\n"
        "\n[Time]\n"
        "  kernel.get_time()                {hours,minutes,seconds} RTC\n"
        "  kernel.get_tsc_ms()              uptime milliseconds via TSC\n"
        "  kernel.sleep_ms(n)               sleep n ms (TSC/PIT fallback)\n"
        "\n[Memory]\n"
        "  kernel.get_ram_mb()              {total_mb, free_mb, used_mb}\n"
        "  kernel.get_heap_stats()          {total, used, free} bytes\n"
        "  kernel.kmalloc_test(size)        alloc+free size bytes, return addr\n"
        "\n================================================\n"
        "Example: kernel.version()   kernel.get_time()\n"
        "In REPL: py> kernel.help()\n"
        "================================================\n\n"
    );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(kernel_help_obj, kernel_help);

// ----------------------------------------------------------------------
// Module registration
// ----------------------------------------------------------------------

static const mp_rom_map_elem_t kernel_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_kernel) },

    { MP_ROM_QSTR(MP_QSTR_print), MP_ROM_PTR(&kernel_print_obj) },
    { MP_ROM_QSTR(MP_QSTR_print_color), MP_ROM_PTR(&kernel_print_color_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&kernel_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_help), MP_ROM_PTR(&kernel_help_obj) },

    { MP_ROM_QSTR(MP_QSTR_version), MP_ROM_PTR(&kernel_version_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_cpu_vendor), MP_ROM_PTR(&kernel_get_cpu_vendor_obj) },
    { MP_ROM_QSTR(MP_QSTR_has_apic), MP_ROM_PTR(&kernel_has_apic_obj) },
    { MP_ROM_QSTR(MP_QSTR_has_sse), MP_ROM_PTR(&kernel_has_sse_obj) },
    { MP_ROM_QSTR(MP_QSTR_has_avx), MP_ROM_PTR(&kernel_has_avx_obj) },

    { MP_ROM_QSTR(MP_QSTR_get_time), MP_ROM_PTR(&kernel_get_time_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_tsc_ms), MP_ROM_PTR(&kernel_get_tsc_ms_obj) },
    { MP_ROM_QSTR(MP_QSTR_sleep_ms), MP_ROM_PTR(&kernel_sleep_ms_obj) },

    { MP_ROM_QSTR(MP_QSTR_get_ram_mb), MP_ROM_PTR(&kernel_get_ram_mb_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_heap_stats), MP_ROM_PTR(&kernel_get_heap_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_kmalloc_test), MP_ROM_PTR(&kernel_kmalloc_test_obj) },
};
static MP_DEFINE_CONST_DICT(kernel_module_globals, kernel_module_globals_table);

const mp_obj_module_t mp_module_kernel = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&kernel_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_kernel, mp_module_kernel);
