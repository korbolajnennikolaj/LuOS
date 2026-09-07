#ifndef ACPI_POWER_H
#define ACPI_POWER_H

#include <stdbool.h>
#include <stdint.h>

bool acpi_find_s5_sleep_type(uint8_t *out_slp_typa, uint8_t *out_slp_typb);

bool acpi_shutdown(void);

void acpi_reboot(void);

#endif
