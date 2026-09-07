#include "tables.h"

#include "checksum.h"
#include "components/Memory/mm.h"
#include "rsdp.h"
#include "xsdt.h"

#include <stdbool.h>
#include <stddef.h>

#define ACPI_RSDT_SIGNATURE "RSDT"

typedef struct
{
    ACPI_SDT_HEADER Header;
    uint32_t Entries[];
} __attribute__((packed)) ACPI_RSDT;

static ACPI_SDT_HEADER *s_cached_tables[ACPI_MAX_CACHED_TABLES];
static int s_cached_count = 0;

static const ACPI_SDT_HEADER *s_root_table = NULL;

static ACPI_RSDT *acpi_load_rsdt(ACPI_RSDP *rsdp) {
    if (!rsdp || rsdp->RsdtAddress == 0) return NULL;

    ACPI_RSDT *rsdt = (ACPI_RSDT *)mm_phys_to_virt(rsdp->RsdtAddress);

    if (!acpi_signature_matches(rsdt->Header.Signature, ACPI_RSDT_SIGNATURE)) {
        return NULL;
    }

    if (!acpi_checksum(rsdt, rsdt->Header.Length)) {
        return NULL;
    }

    return rsdt;
}

static void acpi_cache_add(ACPI_SDT_HEADER *hdr) {
    if (!hdr) return;
    if (s_cached_count >= ACPI_MAX_CACHED_TABLES) return;

    if (!acpi_checksum(hdr, hdr->Length)) {
        return;
    }

    s_cached_tables[s_cached_count++] = hdr;
}

bool acpi_cache_tables(void) {
    s_cached_count = 0;
    s_root_table = NULL;

    ACPI_RSDP *rsdp = acpi_find_rsdp();
    if (!rsdp) return false;

    ACPI_XSDT *xsdt = acpi_load_xsdt(rsdp);
    if (xsdt) {
        s_root_table = &xsdt->Header;

        size_t count = acpi_xsdt_entry_count(xsdt);
        for (size_t i = 0; i < count; i++) {
            uint64_t phys = acpi_xsdt_get_entry(xsdt, i);
            if (!phys) continue;
            acpi_cache_add((ACPI_SDT_HEADER *)mm_phys_to_virt(phys));
        }

        return true;
    }

    ACPI_RSDT *rsdt = acpi_load_rsdt(rsdp);
    if (rsdt) {
        s_root_table = &rsdt->Header;

        size_t count = (rsdt->Header.Length - sizeof(ACPI_SDT_HEADER)) / sizeof(uint32_t);
        for (size_t i = 0; i < count; i++) {
            uint32_t phys = rsdt->Entries[i];
            if (!phys) continue;
            acpi_cache_add((ACPI_SDT_HEADER *)mm_phys_to_virt(phys));
        }

        return true;
    }

    return false;
}

ACPI_SDT_HEADER *acpi_find_table(const char signature[4], int index) {
    if (!signature || index < 0) return NULL;

    int seen = 0;
    for (int i = 0; i < s_cached_count; i++) {
        if (acpi_signature_matches(s_cached_tables[i]->Signature, signature)) {
            if (seen == index) return s_cached_tables[i];
            seen++;
        }
    }

    return NULL;
}

int acpi_table_count(void) {
    return s_cached_count;
}

ACPI_SDT_HEADER *acpi_get_table_by_index(int index) {
    if (index < 0 || index >= s_cached_count) return NULL;
    return s_cached_tables[index];
}

const ACPI_SDT_HEADER *acpi_get_root_table(void) {
    return s_root_table;
}
