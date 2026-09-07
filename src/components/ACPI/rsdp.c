#include "rsdp.h"

#include "checksum.h"
#include "components/Memory/mm.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define RSDP_SIGNATURE "RSD PTR "

static void *acpi_resolve_boot_ptr(uint64_t addr) {
    if (addr == 0) return NULL;

    uint64_t hhdm_off = hhdm_req.response ? hhdm_req.response->offset : 0xffff800000000000ULL;

    if (addr >= hhdm_off) {

        return (void *)addr;
    }

    return (void *)mm_phys_to_virt(addr);
}

static bool rsdp_looks_valid(const ACPI_RSDP *rsdp) {
    if (!rsdp) return false;
    if (memcmp(rsdp->Signature, RSDP_SIGNATURE, 8) != 0) return false;

    if (!acpi_checksum(rsdp, 20)) return false;

    if (rsdp->Revision >= 2) {
        if (!acpi_checksum(rsdp, rsdp->Length ? rsdp->Length : sizeof(ACPI_RSDP))) {
            return false;
        }
    }

    return true;
}

static ACPI_RSDP *acpi_scan_for_rsdp(uint64_t phys_start, uint64_t phys_end) {
    uint8_t *virt = (uint8_t *)mm_phys_to_virt(phys_start);
    uint64_t len = phys_end - phys_start;

    for (uint64_t off = 0; off + sizeof(ACPI_RSDP) <= len; off += 16) {
        ACPI_RSDP *candidate = (ACPI_RSDP *)(virt + off);
        if (rsdp_looks_valid(candidate)) {
            return candidate;
        }
    }

    return NULL;
}

static ACPI_RSDP *acpi_legacy_find_rsdp(void) {

    uint16_t *ebda_seg_ptr = (uint16_t *)mm_phys_to_virt(0x40E);
    uint64_t ebda_phys = ((uint64_t)(*ebda_seg_ptr)) << 4;

    if (ebda_phys != 0) {
        ACPI_RSDP *found = acpi_scan_for_rsdp(ebda_phys, ebda_phys + 1024);
        if (found) return found;
    }

    return acpi_scan_for_rsdp(0xE0000, 0x100000);
}

ACPI_RSDP *acpi_find_rsdp(void) {

    if (rsdp_request.response && rsdp_request.response->address) {
        ACPI_RSDP *rsdp = (ACPI_RSDP *)acpi_resolve_boot_ptr((uint64_t)rsdp_request.response->address);
        if (rsdp_looks_valid(rsdp)) {
            return rsdp;
        }
    }

    return acpi_legacy_find_rsdp();
}
