#ifndef HPET_ACPI_H
#define HPET_ACPI_H

#include "acpi_types.h"

typedef struct
{
    ACPI_SDT_HEADER Header;

    uint32_t EventTimerBlockID;

    ACPI_GAS BaseAddress;

    uint8_t HPETNumber;
    uint16_t MinimumTick;
    uint8_t PageProtection;

} __attribute__((packed)) ACPI_HPET;

#define ACPI_HPET_BLOCKID_REV_ID(id) ((uint8_t)((id) & 0xFF))
#define ACPI_HPET_BLOCKID_COMPARATOR_COUNT(id) ((uint8_t)(((id) >> 8) & 0x1F))
#define ACPI_HPET_BLOCKID_COUNTER_64BIT(id) (((id) >> 13) & 0x1)
#define ACPI_HPET_BLOCKID_LEGACY_CAPABLE(id) (((id) >> 15) & 0x1)
#define ACPI_HPET_BLOCKID_PCI_VENDOR_ID(id) ((uint16_t)(((id) >> 16) & 0xFFFF))

const ACPI_HPET *acpi_get_hpet(void);

uint64_t hpet_acpi_get_physical_address(const ACPI_HPET *hpet);

volatile void *hpet_acpi_get_virtual_address(const ACPI_HPET *hpet);

#endif
