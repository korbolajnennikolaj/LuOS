#include "madt.h"

#include "tables.h"

#include <stddef.h>

static const ACPI_MADT *s_madt = NULL;

static ACPI_MADT_LOCAL_APIC s_cpus[MADT_MAX_CPUS];
static int s_cpu_count = 0;

static ACPI_MADT_IO_APIC s_ioapics[MADT_MAX_IOAPICS];
static int s_ioapic_count = 0;

static ACPI_MADT_INTERRUPT_SOURCE_OVERRIDE s_isos[MADT_MAX_ISOS];
static int s_iso_count = 0;

const ACPI_MADT *acpi_get_madt(void) {
    if (!s_madt) {
        s_madt = (const ACPI_MADT *)acpi_find_table("APIC", 0);
    }
    return s_madt;
}

bool madt_has_legacy_pic(const ACPI_MADT *madt) {
    if (!madt) return false;
    return (madt->Flags & ACPI_MADT_FLAG_PCAT_COMPAT) != 0;
}

const ACPI_MADT_ENTRY *madt_first_entry(const ACPI_MADT *madt) {
    if (!madt) return NULL;
    if (madt->Header.Length <= sizeof(ACPI_MADT)) return NULL;

    const uint8_t *base = (const uint8_t *)madt + sizeof(ACPI_MADT);
    return (const ACPI_MADT_ENTRY *)base;
}

const ACPI_MADT_ENTRY *madt_next_entry(const ACPI_MADT *madt, const ACPI_MADT_ENTRY *cur) {
    if (!madt || !cur) return NULL;
    if (cur->Length == 0) return NULL;

    const uint8_t *madt_end = (const uint8_t *)madt + madt->Header.Length;
    const uint8_t *next = (const uint8_t *)cur + cur->Length;

    if (next + sizeof(ACPI_MADT_ENTRY) > madt_end) return NULL;
    return (const ACPI_MADT_ENTRY *)next;
}

void madt_parse(const ACPI_MADT *madt) {
    s_cpu_count = 0;
    s_ioapic_count = 0;
    s_iso_count = 0;

    if (!madt) return;

    for (const ACPI_MADT_ENTRY *e = madt_first_entry(madt); e; e = madt_next_entry(madt, e)) {
        switch (e->Type) {
            case MADT_ENTRY_LOCAL_APIC:
                if (s_cpu_count < MADT_MAX_CPUS) {
                    s_cpus[s_cpu_count++] = *(const ACPI_MADT_LOCAL_APIC *)e;
                }
                break;

            case MADT_ENTRY_IO_APIC:
                if (s_ioapic_count < MADT_MAX_IOAPICS) {
                    s_ioapics[s_ioapic_count++] = *(const ACPI_MADT_IO_APIC *)e;
                }
                break;

            case MADT_ENTRY_INTERRUPT_SOURCE_OVERRIDE:
                if (s_iso_count < MADT_MAX_ISOS) {
                    s_isos[s_iso_count++] = *(const ACPI_MADT_INTERRUPT_SOURCE_OVERRIDE *)e;
                }
                break;

            default:
                break;
        }
    }
}

int madt_get_cpu_count(void) { return s_cpu_count; }

const ACPI_MADT_LOCAL_APIC *madt_get_cpu(int index) {
    if (index < 0 || index >= s_cpu_count) return NULL;
    return &s_cpus[index];
}

int madt_get_ioapic_count(void) { return s_ioapic_count; }

const ACPI_MADT_IO_APIC *madt_get_ioapic(int index) {
    if (index < 0 || index >= s_ioapic_count) return NULL;
    return &s_ioapics[index];
}

int madt_get_iso_count(void) { return s_iso_count; }

const ACPI_MADT_INTERRUPT_SOURCE_OVERRIDE *madt_get_iso(int index) {
    if (index < 0 || index >= s_iso_count) return NULL;
    return &s_isos[index];
}

uint32_t madt_remap_isa_irq(uint8_t isa_irq) {
    for (int i = 0; i < s_iso_count; i++) {
        if (s_isos[i].Bus == 0 && s_isos[i].Source == isa_irq) {
            return s_isos[i].GlobalSystemInterrupt;
        }
    }
    return isa_irq;
}
