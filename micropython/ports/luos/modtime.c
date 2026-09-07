// modtime.c — the "time" module for MicroPython on LuOS.
//
// Implemented in the style of MicroPython's standard time module
// (ticks_ms/ticks_us/ticks_diff/ticks_add — what embedded ports
// actually use, as opposed to POSIX-style time.time()). The time
// source is the kernel's TSC (kernel.get_tsc_ms() uses the same
// source).
//
// Note: there's no real "epoch" time (Unix timestamp) on LuOS — only
// uptime and the RTC clock (see kernel.get_time()). So time.time()
// here returns seconds since boot, not since 1970 — this is
// explicitly documented in help().

#include "py/obj.h"
#include "py/runtime.h"
#include "py/mphal.h"

#include "mp_kernel.h"

// time.sleep(seconds) — seconds may be a float
static mp_obj_t mod_time_sleep(mp_obj_t seconds_obj) {
    mp_float_t seconds = mp_obj_get_float(seconds_obj);
    if (seconds > 0) {
        mp_hal_delay_ms((mp_uint_t)(seconds * 1000));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_time_sleep_obj, mod_time_sleep);

static mp_obj_t mod_time_sleep_ms(mp_obj_t ms_obj) {
    mp_int_t ms = mp_obj_get_int(ms_obj);
    if (ms > 0) {
        mp_hal_delay_ms((mp_uint_t)ms);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_time_sleep_ms_obj, mod_time_sleep_ms);

static mp_obj_t mod_time_sleep_us(mp_obj_t us_obj) {
    mp_int_t us = mp_obj_get_int(us_obj);
    if (us > 0) {
        mp_hal_delay_us((mp_uint_t)us);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_time_sleep_us_obj, mod_time_sleep_us);

// time.ticks_ms() — monotonic millisecond counter since boot (TSC)
static mp_obj_t mod_time_ticks_ms(void) {
    return mp_obj_new_int_from_uint(mp_kernel_uptime_ms());
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_time_ticks_ms_obj, mod_time_ticks_ms);

static mp_obj_t mod_time_ticks_us(void) {
    return mp_obj_new_int_from_uint(mp_kernel_uptime_ms() * 1000);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_time_ticks_us_obj, mod_time_ticks_us);

// time.ticks_diff(a, b) -> a - b, following MicroPython's typical
// "wraparound" semantics (doesn't actually wrap here — a 64-bit
// TSC-millisecond counter is enough in practice to never overflow).
static mp_obj_t mod_time_ticks_diff(mp_obj_t a_obj, mp_obj_t b_obj) {
    mp_int_t a = mp_obj_get_int(a_obj);
    mp_int_t b = mp_obj_get_int(b_obj);
    return mp_obj_new_int(a - b);
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_time_ticks_diff_obj, mod_time_ticks_diff);

static mp_obj_t mod_time_ticks_add(mp_obj_t a_obj, mp_obj_t delta_obj) {
    mp_int_t a = mp_obj_get_int(a_obj);
    mp_int_t delta = mp_obj_get_int(delta_obj);
    return mp_obj_new_int(a + delta);
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_time_ticks_add_obj, mod_time_ticks_add);

// time.time() -> seconds since kernel boot (NOT Unix epoch, see above)
static mp_obj_t mod_time_time(void) {
    return mp_obj_new_int_from_uint(mp_kernel_uptime_ms() / 1000);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_time_time_obj, mod_time_time);

static const mp_rom_map_elem_t time_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_time) },
    { MP_ROM_QSTR(MP_QSTR_sleep), MP_ROM_PTR(&mod_time_sleep_obj) },
    { MP_ROM_QSTR(MP_QSTR_sleep_ms), MP_ROM_PTR(&mod_time_sleep_ms_obj) },
    { MP_ROM_QSTR(MP_QSTR_sleep_us), MP_ROM_PTR(&mod_time_sleep_us_obj) },
    { MP_ROM_QSTR(MP_QSTR_ticks_ms), MP_ROM_PTR(&mod_time_ticks_ms_obj) },
    { MP_ROM_QSTR(MP_QSTR_ticks_us), MP_ROM_PTR(&mod_time_ticks_us_obj) },
    { MP_ROM_QSTR(MP_QSTR_ticks_diff), MP_ROM_PTR(&mod_time_ticks_diff_obj) },
    { MP_ROM_QSTR(MP_QSTR_ticks_add), MP_ROM_PTR(&mod_time_ticks_add_obj) },
    { MP_ROM_QSTR(MP_QSTR_time), MP_ROM_PTR(&mod_time_time_obj) },
};
static MP_DEFINE_CONST_DICT(time_module_globals, time_module_globals_table);

const mp_obj_module_t mp_module_time = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&time_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_time, mp_module_time);
