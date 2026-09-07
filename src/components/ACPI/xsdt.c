#include "xsdt.h"

#include "checksum.h"
#include "components/Memory/mm.h"

#include <string.h>

ACPI_XSDT *acpi_load_xsdt(ACPI_RSDP *rsdp) {
    if (!rsdp) return NULL;
    if (rsdp->Revision < 2 || rsdp->XsdtAddress == 0) return NULL;

    ACPI_XSDT *xsdt = (ACPI_XSDT *)mm_phys_to_virt(rsdp->XsdtAddress);

    if (!acpi_signature_matches(xsdt->Header.Signature, ACPI_XSDT_SIGNATURE)) {
        return NULL;
    }

    if (!acpi_checksum(xsdt, xsdt->Header.Length)) {
        return NULL;
    }

    return xsdt;
}

size_t acpi_xsdt_entry_count(const ACPI_XSDT *xsdt) {
    if (!xsdt || xsdt->Header.Length < sizeof(ACPI_SDT_HEADER)) return 0;
    return (xsdt->Header.Length - sizeof(ACPI_SDT_HEADER)) / sizeof(uint64_t);
}

uint64_t acpi_xsdt_get_entry(const ACPI_XSDT *xsdt, size_t index) {
    if (index >= acpi_xsdt_entry_count(xsdt)) return 0;
    return xsdt->Entries[index];
}
