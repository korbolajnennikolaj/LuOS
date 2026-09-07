#ifndef XSDT_H
#define XSDT_H

#include "acpi_types.h"
#include "rsdp.h"

#include <stdbool.h>

#define ACPI_XSDT_SIGNATURE "XSDT"

typedef struct
{
    ACPI_SDT_HEADER Header;

    uint64_t Entries[];

} __attribute__((packed)) ACPI_XSDT;

ACPI_XSDT *acpi_load_xsdt(ACPI_RSDP *rsdp);

size_t acpi_xsdt_entry_count(const ACPI_XSDT *xsdt);

uint64_t acpi_xsdt_get_entry(const ACPI_XSDT *xsdt, size_t index);

#endif
