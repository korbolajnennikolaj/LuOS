#ifndef CHECKSUM_H
#define CHECKSUM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool acpi_checksum(const void *table, size_t length);

#endif
