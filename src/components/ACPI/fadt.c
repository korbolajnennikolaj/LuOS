#include "fadt.h"

#include "components/Memory/mm.h"
#include "tables.h"

#include <ports.h>
#include <stddef.h>

static const ACPI_FADT *s_fadt = NULL;

#define FADT_HAS(fadt, field) ((fadt)->Header.Length >= offsetof(ACPI_FADT, field) + sizeof((fadt)->field))

static const ACPI_GAS *fadt_x_gas(const ACPI_GAS *gas, bool present) {
    if (!present || !gas->Address) return NULL;
    if (gas->AddressSpaceID != ACPI_ADDRESS_SPACE_IO && gas->AddressSpaceID != ACPI_ADDRESS_SPACE_MEMORY) return NULL;
    return gas;
}

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
    if (FADT_HAS(fadt, Flags) && (fadt->Flags & ACPI_FADT_HW_REDUCED_ACPI)) return false;
    if (fadt->PM1aControlBlock) return true;
    return fadt_x_gas(&fadt->X_PM1aControlBlock, FADT_HAS(fadt, X_PM1aControlBlock)) != NULL;
}

static uint8_t gas_width(const ACPI_GAS *gas, uint8_t fallback) {
    if (gas->RegisterBitWidth == 8 || gas->RegisterBitWidth == 16 || gas->RegisterBitWidth == 32)
        return gas->RegisterBitWidth;
    if (gas->AccessSize >= 1 && gas->AccessSize <= 3) return (uint8_t)(8u << (gas->AccessSize - 1));
    return fallback;
}

static bool gas_read_w(const ACPI_GAS *gas, uint8_t width, uint32_t *out) {
    if (!gas || gas->Address == 0) return false;
    width = gas_width(gas, width);

    switch (gas->AddressSpaceID) {
        case ACPI_ADDRESS_SPACE_IO: {
            uint16_t port = (uint16_t)gas->Address;
            switch (width) {
                case 8: *out = inb(port); return true;
                case 16: *out = inw(port); return true;
                default: *out = inl(port); return true;
            }
        }
        case ACPI_ADDRESS_SPACE_MEMORY: {
            volatile void *addr = (volatile void *)mm_phys_to_virt(gas->Address);
            switch (width) {
                case 8: *out = *(volatile uint8_t *)addr; return true;
                case 16: *out = *(volatile uint16_t *)addr; return true;
                default: *out = *(volatile uint32_t *)addr; return true;
            }
        }
        default:
            return false;
    }
}

static bool gas_write_w(const ACPI_GAS *gas, uint8_t width, uint32_t value) {
    if (!gas || gas->Address == 0) return false;
    width = gas_width(gas, width);

    switch (gas->AddressSpaceID) {
        case ACPI_ADDRESS_SPACE_IO: {
            uint16_t port = (uint16_t)gas->Address;
            switch (width) {
                case 8: outb(port, (uint8_t)value); return true;
                case 16: outw(port, (uint16_t)value); return true;
                default: outl(port, value); return true;
            }
        }
        case ACPI_ADDRESS_SPACE_MEMORY: {
            volatile void *addr = (volatile void *)mm_phys_to_virt(gas->Address);
            switch (width) {
                case 8: *(volatile uint8_t *)addr = (uint8_t)value; return true;
                case 16: *(volatile uint16_t *)addr = (uint16_t)value; return true;
                default: *(volatile uint32_t *)addr = value; return true;
            }
        }
        case ACPI_ADDRESS_SPACE_PCI_CONFIG: {
            uint8_t dev = (uint8_t)((gas->Address >> 32) & 0x1F);
            uint8_t func = (uint8_t)((gas->Address >> 16) & 0x07);
            uint8_t off = (uint8_t)(gas->Address & 0xFF);
            outl(0xCF8, 0x80000000u | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (off & 0xFC));
            switch (width) {
                case 8: outb((uint16_t)(0xCFC + (off & 3)), (uint8_t)value); return true;
                case 16: outw((uint16_t)(0xCFC + (off & 2)), (uint16_t)value); return true;
                default: outl(0xCFC, value); return true;
            }
        }
        default:
            return false;
    }
}

static bool fadt_pm1_gas(const ACPI_FADT *fadt, int block_b, ACPI_GAS *out) {
    const ACPI_GAS *x = block_b
        ? fadt_x_gas(&fadt->X_PM1bControlBlock, FADT_HAS(fadt, X_PM1bControlBlock))
        : fadt_x_gas(&fadt->X_PM1aControlBlock, FADT_HAS(fadt, X_PM1aControlBlock));
    if (x) {
        *out = *x;
        return true;
    }

    uint32_t port = block_b ? fadt->PM1bControlBlock : fadt->PM1aControlBlock;
    if (!port) return false;
    out->AddressSpaceID = ACPI_ADDRESS_SPACE_IO;
    out->RegisterBitWidth = 16;
    out->RegisterBitOffset = 0;
    out->AccessSize = 2;
    out->Address = port;
    return true;
}

bool fadt_read_pm1_control(const ACPI_FADT *fadt, int block_b, uint16_t *out) {
    ACPI_GAS gas;
    uint32_t v = 0;
    if (!fadt || !out || !fadt_pm1_gas(fadt, block_b, &gas)) return false;
    if (!gas_read_w(&gas, 16, &v)) return false;
    *out = (uint16_t)v;
    return true;
}

bool fadt_write_pm1_control(const ACPI_FADT *fadt, int block_b, uint16_t value) {
    ACPI_GAS gas;
    if (!fadt || !fadt_pm1_gas(fadt, block_b, &gas)) return false;
    return gas_write_w(&gas, 16, value);
}

bool fadt_read_pm_timer(const ACPI_FADT *fadt, uint32_t *out_value) {
    if (!fadt || !out_value) return false;

    const ACPI_GAS *x = fadt_x_gas(&fadt->X_PMTimerBlock, FADT_HAS(fadt, X_PMTimerBlock));
    if (x) return gas_read_w(x, 32, out_value);

    if (fadt->PMTimerBlock != 0) {
        *out_value = inl((uint16_t)fadt->PMTimerBlock);
        return true;
    }

    return false;
}

bool fadt_reset_system(const ACPI_FADT *fadt) {
    if (!fadt) return false;
    if (!FADT_HAS(fadt, ResetValue)) return false;
    if (!(fadt->Flags & ACPI_FADT_RESET_REG_SUP)) return false;
    if (!fadt->ResetReg.Address) return false;

    ACPI_GAS reg = fadt->ResetReg;
    reg.RegisterBitWidth = 8;
    reg.AccessSize = 1;
    return gas_write_w(&reg, 8, fadt->ResetValue);
}

bool fadt_get_smi_command(const ACPI_FADT *fadt, uint16_t *port, uint8_t *enable_value) {
    if (!fadt || !FADT_HAS(fadt, AcpiEnable)) return false;
    if (!fadt->SMICommandPort || !fadt->AcpiEnable) return false;
    *port = (uint16_t)fadt->SMICommandPort;
    *enable_value = fadt->AcpiEnable;
    return true;
}
