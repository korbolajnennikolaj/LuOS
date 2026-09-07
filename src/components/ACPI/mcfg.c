#include "mcfg.h"

#include "tables.h"

#include <stddef.h>

static const ACPI_MCFG *s_mcfg = NULL;

const ACPI_MCFG *acpi_get_mcfg(void) {
    if (!s_mcfg) {
        s_mcfg = (const ACPI_MCFG *)acpi_find_table("MCFG", 0);
    }
    return s_mcfg;
}

size_t mcfg_get_entry_count(const ACPI_MCFG *mcfg) {
    if (!mcfg || mcfg->Header.Length < sizeof(ACPI_MCFG)) return 0;
    return (mcfg->Header.Length - sizeof(ACPI_MCFG)) / sizeof(ACPI_MCFG_ENTRY);
}

const ACPI_MCFG_ENTRY *mcfg_get_entry(const ACPI_MCFG *mcfg, size_t index) {
    if (index >= mcfg_get_entry_count(mcfg)) return NULL;
    return &mcfg->Entries[index];
}

uint64_t mcfg_get_config_address(const ACPI_MCFG *mcfg, uint16_t segment, uint8_t bus, uint8_t device, uint8_t function) {
    size_t count = mcfg_get_entry_count(mcfg);

    for (size_t i = 0; i < count; i++) {
        const ACPI_MCFG_ENTRY *e = &mcfg->Entries[i];
        if (e->SegmentGroup != segment) continue;
        if (bus < e->StartBus || bus > e->EndBus) continue;

        uint64_t bus_offset = (uint64_t)(bus - e->StartBus) << 20;
        uint64_t dev_offset = (uint64_t)device << 15;
        uint64_t fn_offset = (uint64_t)function << 12;

        return e->BaseAddress + bus_offset + dev_offset + fn_offset;
    }

    return 0;
}
