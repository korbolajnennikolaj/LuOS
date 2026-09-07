#ifndef TABLES_H
#define TABLES_H

#include "acpi_types.h"

#include <stdbool.h>

#define ACPI_MAX_CACHED_TABLES 32

ACPI_SDT_HEADER *acpi_find_table(const char signature[4], int index);

bool acpi_cache_tables(void);

int acpi_table_count(void);

ACPI_SDT_HEADER *acpi_get_table_by_index(int index);

const ACPI_SDT_HEADER *acpi_get_root_table(void);

#endif
