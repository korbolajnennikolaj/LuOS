#include "acpi.h"

#include "fadt.h"
#include "hpet_acpi.h"
#include "madt.h"
#include "mcfg.h"
#include "rsdp.h"
#include "tables.h"
#include "xsdt.h"

#include <stddef.h>
#include <stdio.h>

static bool s_acpi_ready = false;
static bool s_acpi_init_attempted = false;

static ACPI_RSDP *s_rsdp = NULL;

bool acpi_init(void) {
    if (s_acpi_init_attempted) {
        return s_acpi_ready;
    }
    s_acpi_init_attempted = true;

    s_rsdp = acpi_find_rsdp();
    if (!s_rsdp) {
        s_acpi_ready = false;
        return false;
    }

    if (!acpi_cache_tables()) {
        s_acpi_ready = false;
        return false;
    }

    const ACPI_MADT *madt = acpi_get_madt();
    if (madt) madt_parse(madt);

    acpi_get_fadt();
    acpi_get_mcfg();
    acpi_get_hpet();

    s_acpi_ready = true;
    return true;
}

bool acpi_is_available(void) {
    return s_acpi_ready;
}

const void *acpi_get_rsdp(void) {
    return s_rsdp;
}

const void *acpi_get_xsdt(void) {
    return acpi_get_root_table();
}

void acpi_print_summary(void) {
    if (!s_acpi_init_attempted) {
        printf("[ACPI] acpi_init() has not been called yet\n");
        return;
    }

    if (!s_acpi_ready) {
        printf("[ACPI] no usable ACPI tables were found\n");
        return;
    }

    printf("[ACPI] RSDP revision %u, root table \"%.4s\", %d tables cached\n",
           s_rsdp->Revision,
           acpi_get_root_table() ? acpi_get_root_table()->Signature : "????",
           acpi_table_count());

    const ACPI_MADT *madt = acpi_get_madt();
    if (madt) {
        printf("[ACPI] MADT: %d CPU(s), %d IOAPIC(s), %d IRQ override(s), legacy PIC: %s\n",
               madt_get_cpu_count(), madt_get_ioapic_count(), madt_get_iso_count(),
               madt_has_legacy_pic(madt) ? "yes" : "no");
    } else {
        printf("[ACPI] MADT not present\n");
    }

    const ACPI_FADT *fadt = acpi_get_fadt();
    printf("[ACPI] FADT: %s\n", fadt ? "present" : "not present");

    const ACPI_MCFG *mcfg = acpi_get_mcfg();
    if (mcfg) {
        printf("[ACPI] MCFG: %d PCIe segment group(s)\n", (int)mcfg_get_entry_count(mcfg));
    } else {
        printf("[ACPI] MCFG not present (PCI will use legacy 0xCF8/0xCFC access)\n");
    }

    const ACPI_HPET *hpet = acpi_get_hpet();
    if (hpet) {
        printf("[ACPI] HPET: physical base 0x%lx\n", hpet_acpi_get_physical_address(hpet));
    } else {
        printf("[ACPI] HPET table not present\n");
    }
}
