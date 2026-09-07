// modrandom.c — the "random" module for MicroPython on LuOS.
//
// There's nowhere to get a real /dev/urandom from here (no kernel
// entropy, no FS), so a fast software PRNG (xorshift64*) is used,
// auto-seeded from TSC uptime on first use. Good enough for "basic"
// scenarios (games, tests, demo scripts); not suitable for
// cryptography — it's a plain pseudorandom generator, just like the
// standard random module in CPython/MicroPython.

#include "py/obj.h"
#include "py/runtime.h"

#include "mp_kernel.h"

static uint64_t rng_state = 0;
static int rng_seeded = 0;

static void rng_ensure_seeded(void) {
    if (!rng_seeded) {
        uint64_t seed = mp_kernel_uptime_ms();
        // In case TSC isn't ready yet (uptime == 0) — use a fixed
        // nonzero constant so xorshift doesn't degenerate to 0.
        rng_state = seed ? seed : 0x9E3779B97F4A7C15ULL;
        rng_seeded = 1;
    }
}

static uint64_t xorshift64star(void) {
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1DULL;
}

// random.seed([n]) — with no argument, reseeds from the current uptime (TSC).
static mp_obj_t mod_random_seed(size_t n_args, const mp_obj_t *args) {
    if (n_args == 0) {
        rng_state = mp_kernel_uptime_ms() ^ 0x9E3779B97F4A7C15ULL;
    } else {
        mp_int_t s = mp_obj_get_int(args[0]);
        rng_state = (uint64_t)s ^ 0x9E3779B97F4A7C15ULL;
    }
    if (rng_state == 0) rng_state = 1; // xorshift doesn't survive a zero state
    rng_seeded = 1;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_random_seed_obj, 0, 1, mod_random_seed);

// random.random() -> float in [0.0, 1.0)
static mp_obj_t mod_random_random(void) {
    rng_ensure_seeded();
    uint64_t r = xorshift64star();
    // The top 53 bits — the same as CPython does for double precision.
    double d = (double)(r >> 11) * (1.0 / 9007199254740992.0);
    return mp_obj_new_float(d);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_random_random_obj, mod_random_random);

// random.getrandbits(n) -> integer from the n low-order random bits (n <= 64)
static mp_obj_t mod_random_getrandbits(mp_obj_t n_obj) {
    rng_ensure_seeded();
    mp_int_t n = mp_obj_get_int(n_obj);
    if (n <= 0 || n > 64) {
        mp_raise_ValueError(MP_ERROR_TEXT("bits must be 1..64"));
    }
    uint64_t r = xorshift64star();
    if (n < 64) {
        r &= ((uint64_t)1 << n) - 1;
    }
    return mp_obj_new_int_from_uint(r);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_random_getrandbits_obj, mod_random_getrandbits);

// random.randint(a, b) -> integer in [a, b] inclusive
static mp_obj_t mod_random_randint(mp_obj_t a_obj, mp_obj_t b_obj) {
    rng_ensure_seeded();
    mp_int_t a = mp_obj_get_int(a_obj);
    mp_int_t b = mp_obj_get_int(b_obj);
    if (b < a) {
        mp_raise_ValueError(MP_ERROR_TEXT("b < a in randint"));
    }
    uint64_t range = (uint64_t)(b - a) + 1;
    uint64_t r = xorshift64star() % range;
    return mp_obj_new_int(a + (mp_int_t)r);
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_random_randint_obj, mod_random_randint);

// random.randrange(n) -> integer in [0, n)
static mp_obj_t mod_random_randrange(mp_obj_t n_obj) {
    rng_ensure_seeded();
    mp_int_t n = mp_obj_get_int(n_obj);
    if (n <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("randrange requires n > 0"));
    }
    uint64_t r = xorshift64star() % (uint64_t)n;
    return mp_obj_new_int((mp_int_t)r);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_random_randrange_obj, mod_random_randrange);

// random.uniform(a, b) -> float in [a, b)
static mp_obj_t mod_random_uniform(mp_obj_t a_obj, mp_obj_t b_obj) {
    rng_ensure_seeded();
    double a = mp_obj_get_float(a_obj);
    double b = mp_obj_get_float(b_obj);
    uint64_t r = xorshift64star();
    double f = (double)(r >> 11) * (1.0 / 9007199254740992.0);
    return mp_obj_new_float(a + (b - a) * f);
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_random_uniform_obj, mod_random_uniform);

// random.choice(seq) -> a random element from a sequence with __len__/__getitem__
static mp_obj_t mod_random_choice(mp_obj_t seq_obj) {
    rng_ensure_seeded();
    mp_int_t len = mp_obj_get_int(mp_obj_len(seq_obj));
    if (len <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("choice from empty sequence"));
    }
    uint64_t idx = xorshift64star() % (uint64_t)len;
    return mp_obj_subscr(seq_obj, mp_obj_new_int((mp_int_t)idx), MP_OBJ_SENTINEL);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_random_choice_obj, mod_random_choice);

static const mp_rom_map_elem_t random_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_random) },
    { MP_ROM_QSTR(MP_QSTR_seed), MP_ROM_PTR(&mod_random_seed_obj) },
    { MP_ROM_QSTR(MP_QSTR_random), MP_ROM_PTR(&mod_random_random_obj) },
    { MP_ROM_QSTR(MP_QSTR_getrandbits), MP_ROM_PTR(&mod_random_getrandbits_obj) },
    { MP_ROM_QSTR(MP_QSTR_randint), MP_ROM_PTR(&mod_random_randint_obj) },
    { MP_ROM_QSTR(MP_QSTR_randrange), MP_ROM_PTR(&mod_random_randrange_obj) },
    { MP_ROM_QSTR(MP_QSTR_uniform), MP_ROM_PTR(&mod_random_uniform_obj) },
    { MP_ROM_QSTR(MP_QSTR_choice), MP_ROM_PTR(&mod_random_choice_obj) },
};
static MP_DEFINE_CONST_DICT(random_module_globals, random_module_globals_table);

const mp_obj_module_t mp_module_random = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&random_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_random, mp_module_random);
