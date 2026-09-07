#include "py/obj.h"
#include "py/runtime.h"
#include "components/ACPI/power.h" // acpi_reboot()
#include "components/Memory/heap.h" // heap_get_used()

// Python: luos.reboot()
static mp_obj_t luos_reboot(void) {
    acpi_reboot();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(luos_reboot_obj, luos_reboot);

// Python: luos.mem_info()
static mp_obj_t luos_mem_info(void) {
    mp_printf(&mp_plat_print, "Kernel Heap Used: %u bytes\n", heap_get_used());
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(luos_mem_info_obj, luos_mem_info);

// Module registration
static const mp_rom_map_elem_t luos_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_luos) },
    { MP_ROM_QSTR(MP_QSTR_reboot), MP_ROM_PTR(&luos_reboot_obj) },
    { MP_ROM_QSTR(MP_QSTR_mem_info), MP_ROM_PTR(&luos_mem_info_obj) },
};
static MP_DEFINE_CONST_DICT(luos_module_globals, luos_module_globals_table);

const mp_obj_module_t mp_module_luos = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&luos_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_luos, mp_module_luos);
