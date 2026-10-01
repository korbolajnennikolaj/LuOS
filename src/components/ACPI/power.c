#include "power.h"

#include "acpi.h"
#include "acpi_types.h"
#include "checksum.h"
#include "components/logger.h"
#include "components/Memory/mm.h"
#include "fadt.h"
#include "tables.h"

#include <ports.h>
#include <stddef.h>

static bool acpi_parse_s5_in_bytes(const uint8_t *bytes, uint32_t len, uint8_t *out_a, uint8_t *out_b) {
    if (!bytes || len < 9) return false;

    for (uint32_t i = 0; i + 5 < len; i++) {
        if (!(bytes[i] == '_' && bytes[i + 1] == 'S' && bytes[i + 2] == '5' && bytes[i + 3] == '_')) {
            continue;
        }

        const uint8_t *p = bytes + i;

        bool prefixed = (i >= 1 && p[-1] == 0x08) ||
                         (i >= 2 && p[-2] == 0x08 && p[-1] == '\\');
        if (!prefixed) continue;

        if (p[4] != 0x12) continue;

        const uint8_t *q = p + 5;
        uint32_t extra_len_bytes = (q[0] & 0xC0) >> 6;
        q += extra_len_bytes + 2;

        if (bytes + len <= q + 1) continue;

        if (*q == 0x0A) q++;
        uint8_t typa = *q;
        q++;

        if (bytes + len <= q) continue;
        if (*q == 0x0A) q++;
        uint8_t typb = *q;

        *out_a = typa;
        *out_b = typb;
        return true;
    }

    return false;
}

bool acpi_find_s5_sleep_type(uint8_t *out_slp_typa, uint8_t *out_slp_typb) {
    if (!out_slp_typa || !out_slp_typb) return false;

    const ACPI_FADT *fadt = acpi_get_fadt();
    uint64_t dsdt_phys = fadt_get_dsdt_address(fadt);

    if (dsdt_phys) {
        const ACPI_SDT_HEADER *dsdt = (const ACPI_SDT_HEADER *)mm_phys_to_virt(dsdt_phys);

        if (dsdt && acpi_signature_matches(dsdt->Signature, "DSDT") &&
            dsdt->Length >= sizeof(ACPI_SDT_HEADER) &&
            acpi_checksum(dsdt, dsdt->Length)) {
            const uint8_t *body = (const uint8_t *)dsdt + sizeof(ACPI_SDT_HEADER);
            uint32_t body_len = dsdt->Length - sizeof(ACPI_SDT_HEADER);

            if (acpi_parse_s5_in_bytes(body, body_len, out_slp_typa, out_slp_typb)) {
                return true;
            }
        }
    }

    for (int i = 0; i < acpi_table_count(); i++) {
        ACPI_SDT_HEADER *hdr = acpi_get_table_by_index(i);
        if (!hdr || !acpi_signature_matches(hdr->Signature, "SSDT")) continue;

        const uint8_t *body = (const uint8_t *)hdr + sizeof(ACPI_SDT_HEADER);
        uint32_t body_len = hdr->Length > sizeof(ACPI_SDT_HEADER) ? hdr->Length - sizeof(ACPI_SDT_HEADER) : 0;

        if (acpi_parse_s5_in_bytes(body, body_len, out_slp_typa, out_slp_typb)) {
            return true;
        }
    }

    return false;
}

static uint16_t read_pm1a_control(const ACPI_FADT *fadt) {
    if (fadt->X_PM1aControlBlock.Address) return inw((uint16_t)fadt->X_PM1aControlBlock.Address);
    if (fadt->PM1aControlBlock) return inw((uint16_t)fadt->PM1aControlBlock);
    return 0;
}

static void acpi_enable_if_needed(const ACPI_FADT *fadt) {
    if (read_pm1a_control(fadt) & ACPI_PM1_CNT_SCI_EN) {
        return;
    }

    if (!fadt->SMICommandPort || !fadt->AcpiEnable) {
        LOG_WARNING("SCI_EN is clear but FADT has no SMI command to enable ACPI mode");
        return;
    }

    LOG_DEBUG("switching to ACPI mode via SMI port 0x%x", (unsigned)fadt->SMICommandPort);
    outb((uint16_t)fadt->SMICommandPort, fadt->AcpiEnable);

    for (volatile int i = 0; i < 1000000; i++) {
        if (read_pm1a_control(fadt) & ACPI_PM1_CNT_SCI_EN) break;
    }

    if (!(read_pm1a_control(fadt) & ACPI_PM1_CNT_SCI_EN))
        LOG_WARNING("ACPI mode enable did not set SCI_EN");
}

bool acpi_shutdown(void) {
    if (!acpi_init()) {
        LOG_ERROR("shutdown failed: ACPI not available");
        return false;
    }

    const ACPI_FADT *fadt = acpi_get_fadt();
    if (!fadt || !fadt_has_fixed_hardware(fadt)) {
        LOG_ERROR("shutdown failed: no FADT / fixed hardware PM1 block");
        return false;
    }

    uint8_t slp_typa, slp_typb;
    if (!acpi_find_s5_sleep_type(&slp_typa, &slp_typb)) {
        LOG_ERROR("shutdown failed: could not find \\_S5 in DSDT/SSDT");
        return false;
    }

    acpi_enable_if_needed(fadt);

    LOG_INFO("entering S5 (SLP_TYPa=%u SLP_TYPb=%u)", (unsigned)slp_typa, (unsigned)slp_typb);

    uint16_t val_a = (uint16_t)((slp_typa << ACPI_PM1_CNT_SLP_TYP_SHIFT) | ACPI_PM1_CNT_SLP_EN);
    uint16_t val_b = (uint16_t)((slp_typb << ACPI_PM1_CNT_SLP_TYP_SHIFT) | ACPI_PM1_CNT_SLP_EN);

    if (fadt->X_PM1aControlBlock.Address) {
        outw((uint16_t)fadt->X_PM1aControlBlock.Address, val_a);
    } else if (fadt->PM1aControlBlock) {
        outw((uint16_t)fadt->PM1aControlBlock, val_a);
    }

    if (fadt->X_PM1bControlBlock.Address) {
        outw((uint16_t)fadt->X_PM1bControlBlock.Address, val_b);
    } else if (fadt->PM1bControlBlock) {
        outw((uint16_t)fadt->PM1bControlBlock, val_b);
    }

    for (;;) {
        asm volatile("cli; hlt");
    }
}

static void reboot_via_8042(void) {

    for (int timeout = 0; timeout < 100000; timeout++) {
        if ((inb(0x64) & 0x02) == 0) break;
    }
    outb(0x64, 0xFE);
}

static void reboot_via_triple_fault(void) {
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = { 0, 0 };
    asm volatile("lidt %0" :: "m"(null_idt));
    asm volatile("int3");
}

void acpi_reboot(void) {
    acpi_init();

    const ACPI_FADT *fadt = acpi_get_fadt();
    if (fadt && (fadt->Flags & ACPI_FADT_RESET_REG_SUP)) {
        LOG_INFO("resetting via FADT reset register");
        fadt_reset_system(fadt);
    }

    LOG_WARNING("FADT reset unavailable/ineffective, trying 8042 controller");
    reboot_via_8042();

    for (volatile int i = 0; i < 10000000; i++) { }

    LOG_WARNING("8042 reset ineffective, forcing a triple fault");
    reboot_via_triple_fault();

    for (;;) {
        asm volatile("cli; hlt");
    }
}
