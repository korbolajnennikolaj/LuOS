#include "fadt.h"

#include "components/Memory/mm.h"
#include "tables.h"

#include <ports.h>
#include <stddef.h>

static const ACPI_FADT *s_fadt = NULL;

const ACPI_FADT *acpi_get_fadt(void) {
    if (!s_fadt) {
        s_fadt = (const ACPI_FADT *)acpi_find_table("FACP", 0);
    }
    return s_fadt;
}

uint64_t fadt_get_dsdt_address(const ACPI_FADT *fadt) {
    if (!fadt) return 0;

    size_t need = offsetof(ACPI_FADT, X_Dsdt) + sizeof(fadt->X_Dsdt);
    if (fadt->Header.Length >= need && fadt->X_Dsdt) return fadt->X_Dsdt;

    return fadt->Dsdt;
}

bool fadt_has_fixed_hardware(const ACPI_FADT *fadt) {
    if (!fadt) return false;
    if (fadt->Flags & ACPI_FADT_HW_REDUCED_ACPI) return false;
    return (fadt->X_PM1aEventBlock.Address != 0) || (fadt->PM1aEventBlock != 0);
}

static bool gas_read(const ACPI_GAS *gas, uint32_t *out) {
    if (!gas || gas->Address == 0) return false;

    switch (gas->AddressSpaceID) {
        case ACPI_ADDRESS_SPACE_IO: {
            uint16_t port = (uint16_t)gas->Address;
            switch (gas->RegisterBitWidth) {
                case 8: *out = inb(port); return true;
                case 16: *out = inw(port); return true;
                default: *out = inl(port); return true;
            }
        }
        case ACPI_ADDRESS_SPACE_MEMORY: {
            volatile void *addr = (volatile void *)mm_phys_to_virt(gas->Address);
            switch (gas->RegisterBitWidth) {
                case 8: *out = *(volatile uint8_t *)addr; return true;
                case 16: *out = *(volatile uint16_t *)addr; return true;
                default: *out = *(volatile uint32_t *)addr; return true;
            }
        }
        default:
            return false;
    }
}

static bool gas_write(const ACPI_GAS *gas, uint32_t value) {
    if (!gas || gas->Address == 0) return false;

    switch (gas->AddressSpaceID) {
        case ACPI_ADDRESS_SPACE_IO: {
            uint16_t port = (uint16_t)gas->Address;
            switch (gas->RegisterBitWidth) {
                case 8: outb(port, (uint8_t)value); return true;
                case 16: outw(port, (uint16_t)value); return true;
                default: outl(port, value); return true;
            }
        }
        case ACPI_ADDRESS_SPACE_MEMORY: {
            volatile void *addr = (volatile void *)mm_phys_to_virt(gas->Address);
            switch (gas->RegisterBitWidth) {
                case 8: *(volatile uint8_t *)addr = (uint8_t)value; return true;
                case 16: *(volatile uint16_t *)addr = (uint16_t)value; return true;
                default: *(volatile uint32_t *)addr = value; return true;
            }
        }
        default:
            return false;
    }
}

bool fadt_read_pm_timer(const ACPI_FADT *fadt, uint32_t *out_value) {
    if (!fadt || !out_value) return false;

    if (fadt->X_PMTimerBlock.Address != 0) {
        return gas_read(&fadt->X_PMTimerBlock, out_value);
    }

    if (fadt->PMTimerBlock != 0) {
        *out_value = inl((uint16_t)fadt->PMTimerBlock);
        return true;
    }

    return false;
}

void fadt_reset_system(const ACPI_FADT *fadt) {
    if (!fadt) return;
    if (!(fadt->Flags & ACPI_FADT_RESET_REG_SUP)) return;

    gas_write(&fadt->ResetReg, fadt->ResetValue);
}
