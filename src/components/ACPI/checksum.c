#include "checksum.h"

#include <stdint.h>

bool acpi_checksum(const void *table, size_t length) {
    if (!table || length == 0) return false;

    const uint8_t *bytes = (const uint8_t *)table;
    uint8_t sum = 0;

    for (size_t i = 0; i < length; i++) {
        sum = (uint8_t)(sum + bytes[i]);
    }

    return sum == 0;
}
