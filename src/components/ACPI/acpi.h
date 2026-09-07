#ifndef ACPI_H
#define ACPI_H

#include "acpi_types.h"

#include <stdbool.h>

bool acpi_init(void);

bool acpi_is_available(void);

void acpi_print_summary(void);

ACPI_SDT_HEADER *acpi_find_table(const char signature[4], int index);

const void *acpi_get_rsdp(void);
const void *acpi_get_xsdt(void);

#endif
