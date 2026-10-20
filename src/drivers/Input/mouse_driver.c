// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

/*
 *                                             MOUSE STACK
 *
 *                       +----------------+   +---------+   +--------------------+
 *                       |    console     |   | games.c |   | lua + micropython  |
 *                       | cursor dx / dy |   | buttons |   | position / buttons |
 *                       +----------------+   +---------+   +--------------------+
 *                                |                |                   |
 *                                +----------------+-------------------+
 *                                                   |
 *                                                   |
 *                            +=============================================+
 *                            |         m o u s e _ d r i v e r . c         |
 *                            +---------------------------------------------+
 *                            | state     x / y / wheel / btn L R M         |
 *                            | accum     dx / dy / wheel over all backends |
 *                            | deltas    reset after each consumer read    |
 *                            | backends  ps2 + usb, hotplug set            |
 *                            | pump      service_mouse_updater polls       |
 *                            +=============================================+
 *                                                   |
 *                                                   |
 *                                      +------------+--------------+
 *                                      |                           |
 *                          +-----------------------+     +------------------+
 *                          |    ps2_mouse_driver   |     | usb_mouse_driver |
 *                          | i8042 + IRQ12         |     | HID boot         |
 *                          | 3 / 4 byte packets    |     | rpt 1: btn dx dy |
 *                          | wheel: 200-100-80-200 |     | rpt 2: + wheel   |
 *                          +-----------------------+     | 8 ms interval    |
 *                                      |                 +------------------+
 *                                      |                           |
 *                                      +---------------------------+
 *                                                   |
 *                         +--------------------------------------------------+
 *                         | i8042 ports 0x60 / 0x64   |   USB HCD (usb_core) |
 *                         +--------------------------------------------------+
 *
 *   move:  IRQ -> packet -> decode -> state -> cursor / game
 *   wheel: 4th byte, 0x08 sign bit -> +/- 1 notch
 *   plug:  usb claim -> register_backend -> active set
 */

#include "drivers/Input/mouse_driver.h"

#include "components/drivers.h"
#include "components/logger.h"

#include <stddef.h>

static struct mouse_driver vmouse;

#define FOR_EACH_BACKEND(idx) \
    for (uint8_t idx = 0; idx < vmouse.backend_count; idx++)

static struct mouse_state vmouse_state = {0};

static void backend_poll(struct mouse_backend *b) {
    if (!b->active) return;

    if (b->type == USB_MOUSE && b->drv.usb && b->drv.usb->mouse_handler) {
        b->drv.usb->mouse_handler();
        return;
    }

    if (b->type == PS2_MOUSE && b->drv.ps2 && b->drv.ps2->mouse_handler)
        b->drv.ps2->mouse_handler();
}

static void backend_reset_deltas(struct mouse_backend *b) {
    if (!b->active) return;
    if (b->type == PS2_MOUSE && b->drv.ps2 && b->drv.ps2->reset_deltas)
        b->drv.ps2->reset_deltas();
    else if (b->type == USB_MOUSE && b->drv.usb && b->drv.usb->reset_deltas)
        b->drv.usb->reset_deltas();
}

static void accumulate_backends(void) {
    vmouse_state.x = 0;
    vmouse_state.y = 0;
    vmouse_state.wheel = 0;
    vmouse_state.btn_left = false;
    vmouse_state.btn_right = false;
    vmouse_state.btn_middle = false;

    FOR_EACH_BACKEND(i) {
        struct mouse_backend *b = &vmouse.backends[i];
        if (!b->active) continue;

        if (b->type == PS2_MOUSE && b->drv.ps2 && b->drv.ps2->get_state) {
            struct ps2_mouse_state *s = b->drv.ps2->get_state();
            if (!s) continue;
            vmouse_state.x += s->x;
            vmouse_state.y += s->y;
            vmouse_state.wheel += s->wheel;
            vmouse_state.btn_left |= s->btn_left;
            vmouse_state.btn_right |= s->btn_right;
            vmouse_state.btn_middle |= s->btn_middle;
        }

        if (b->type == USB_MOUSE && b->drv.usb && b->drv.usb->get_state) {
            struct usb_mouse_state *s = b->drv.usb->get_state();
            if (!s) continue;
            vmouse_state.x += s->x;
            vmouse_state.y += s->y;
            vmouse_state.wheel += s->wheel;
            vmouse_state.btn_left |= s->btn_left;
            vmouse_state.btn_right |= s->btn_right;
            vmouse_state.btn_middle |= s->btn_middle;
        }
    }
}

static mouse_state_t *vmouse_get_state(void) {
    accumulate_backends();
    return &vmouse_state;
}

static void vmouse_reset_deltas(void) {
    FOR_EACH_BACKEND(i)
        backend_reset_deltas(&vmouse.backends[i]);
    vmouse_state.x = 0;
    vmouse_state.y = 0;
    vmouse_state.wheel = 0;
}

static void vmouse_mouse_handler(void) {

    FOR_EACH_BACKEND(i) backend_poll(&vmouse.backends[i]);
}

static int vmouse_get_active_type(void) {

    FOR_EACH_BACKEND(i) {
        struct mouse_backend *b = &vmouse.backends[i];

        if (b->active && b->type == USB_MOUSE && b->drv.usb &&
            b->drv.usb->mouse_count && b->drv.usb->mouse_count() > 0)
            return USB_MOUSE;
    }
    FOR_EACH_BACKEND(i) {
        struct mouse_backend *b = &vmouse.backends[i];
        if (b->active && b->type == PS2_MOUSE && b->drv.ps2) return PS2_MOUSE;
    }
    return -1;
}

static int vmouse_register_backend(enum MOUSE_TYPE type, void *drv)
{
    if (!drv || vmouse.backend_count >= MOUSE_MAX_BACKENDS) {
        LOG_WARNING("cannot register %s mouse backend (%s)",
                    type == PS2_MOUSE ? "PS/2" : "USB", drv ? "table full" : "NULL driver");
        return -1;
    }
    struct mouse_backend *b = &vmouse.backends[vmouse.backend_count];
    b->type = type;
    b->active = true;
    if (type == PS2_MOUSE)
        b->drv.ps2 = (struct ps2_mouse_driver *)drv;
    else
        b->drv.usb = (struct usb_mouse_driver *)drv;
    LOG_DEBUG("backend %u: %s mouse", (unsigned)vmouse.backend_count, type == PS2_MOUSE ? "PS/2" : "USB");
    return (int)vmouse.backend_count++;
}

static struct mouse_driver vmouse = {
    .backend_count = 0,
    .get_state = vmouse_get_state,
    .reset_deltas = vmouse_reset_deltas,
    .mouse_handler = vmouse_mouse_handler,
    .register_backend = vmouse_register_backend,
    .get_active_type = vmouse_get_active_type,
};

struct mouse_driver *return_mouse_driver(void) {
    vmouse.backend_count = 0;
    struct ps2_mouse_driver *ps2 =
        (struct ps2_mouse_driver *)get_self_driver(MOUSE_DRIVER, PS2_MOUSE);
    if (ps2) vmouse.register_backend(PS2_MOUSE, ps2);
    struct usb_mouse_driver *usb =
        (struct usb_mouse_driver *)get_self_driver(MOUSE_DRIVER, USB_MOUSE);
    if (usb) vmouse.register_backend(USB_MOUSE, usb);
    if (vmouse.backend_count == 0)
        LOG_WARNING("no mouse backends available");
    else
        LOG_INFO("virtual mouse ready with %u backend(s)", (unsigned)vmouse.backend_count);
    return &vmouse;
}

struct driver *return_meta_mouse_driver(void) {
    static struct dependency dep[] = {
        MAKE_DEPENDENCY(MOUSE_DRIVER, USB_MOUSE),
        MAKE_DEPENDENCY(MOUSE_DRIVER, PS2_MOUSE),
    };
    static struct driver meta = {
        .name = "Virtual Mouse Driver",
        .type = MOUSE_DRIVER,
        .sub_type = VIRTUAL_MOUSE,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { &dep[0], &dep[1] },
        .dependency_count = 2,
        .self = &vmouse,
        .init = (void *)return_mouse_driver,
    };
    return &meta;
}
