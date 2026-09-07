#ifndef MCFG_H
#define MCFG_H

#include "acpi_types.h"

typedef struct
{
    uint64_t BaseAddress;
    uint16_t SegmentGroup;
    uint8_t StartBus;
    uint8_t EndBus;
    uint32_t Reserved;
} __attribute__((packed)) ACPI_MCFG_ENTRY;

typedef struct
{
    ACPI_SDT_HEADER Header;

    uint64_t Reserved;

    ACPI_MCFG_ENTRY Entries[];

} __attribute__((packed)) ACPI_MCFG;

const ACPI_MCFG *acpi_get_mcfg(void);

size_t mcfg_get_entry_count(const ACPI_MCFG *mcfg);

const ACPI_MCFG_ENTRY *mcfg_get_entry(const ACPI_MCFG *mcfg, size_t index);

uint64_t mcfg_get_config_address(const ACPI_MCFG *mcfg, uint16_t segment, uint8_t bus, uint8_t device, uint8_t function);

#endif
