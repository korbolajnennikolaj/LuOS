#include "hpet_acpi.h"

#include "components/Memory/mm.h"
#include "tables.h"

#include <stddef.h>

static const ACPI_HPET *s_hpet = NULL;

const ACPI_HPET *acpi_get_hpet(void) {
    if (!s_hpet) {
        s_hpet = (const ACPI_HPET *)acpi_find_table("HPET", 0);
    }
    return s_hpet;
}

uint64_t hpet_acpi_get_physical_address(const ACPI_HPET *hpet) {
    if (!hpet) return 0;
    return hpet->BaseAddress.Address;
}

volatile void *hpet_acpi_get_virtual_address(const ACPI_HPET *hpet) {
    uint64_t phys = hpet_acpi_get_physical_address(hpet);
    if (!phys) return NULL;
    return (volatile void *)mm_phys_to_virt(phys);
}
