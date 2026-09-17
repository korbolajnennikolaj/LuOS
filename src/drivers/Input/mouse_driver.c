#include "drivers/Input/mouse_driver.h"

#include "components/drivers.h"

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
    if (!drv || vmouse.backend_count >= MOUSE_MAX_BACKENDS) return -1;
    struct mouse_backend *b = &vmouse.backends[vmouse.backend_count];
    b->type = type;
    b->active = true;
    if (type == PS2_MOUSE)
        b->drv.ps2 = (struct ps2_mouse_driver *)drv;
    else
        b->drv.usb = (struct usb_mouse_driver *)drv;
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
