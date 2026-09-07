#ifndef MADT_H
#define MADT_H

#include "acpi_types.h"

#include <stdbool.h>

typedef struct
{
    ACPI_SDT_HEADER Header;

    uint32_t LocalAPICAddress;
    uint32_t Flags;

} __attribute__((packed)) ACPI_MADT;

#define ACPI_MADT_FLAG_PCAT_COMPAT (1u << 0)

typedef struct {
    uint8_t Type;
    uint8_t Length;
} ACPI_MADT_ENTRY;

typedef enum {
    MADT_ENTRY_LOCAL_APIC = 0,
    MADT_ENTRY_IO_APIC = 1,
    MADT_ENTRY_INTERRUPT_SOURCE_OVERRIDE = 2,
    MADT_ENTRY_NMI_SOURCE = 3,
    MADT_ENTRY_LOCAL_APIC_NMI = 4,
    MADT_ENTRY_LOCAL_APIC_ADDRESS_OVERRIDE = 5,
    MADT_ENTRY_LOCAL_X2APIC = 9,
    MADT_ENTRY_LOCAL_X2APIC_NMI = 0xA,
} ACPI_MADT_ENTRY_TYPE;

#define ACPI_MADT_LAPIC_FLAG_ENABLED (1u << 0)
#define ACPI_MADT_LAPIC_FLAG_ONLINE_CAPABLE (1u << 1)

typedef struct {
    ACPI_MADT_ENTRY Entry;
    uint8_t ProcessorID;
    uint8_t APICID;
    uint32_t Flags;
} __attribute__((packed)) ACPI_MADT_LOCAL_APIC;

typedef struct {
    ACPI_MADT_ENTRY Entry;
    uint8_t IOAPICID;
    uint8_t Reserved;
    uint32_t IOAPICAddress;
    uint32_t GlobalSystemInterruptBase;
} __attribute__((packed)) ACPI_MADT_IO_APIC;

typedef struct {
    ACPI_MADT_ENTRY Entry;
    uint8_t Bus;
    uint8_t Source;
    uint32_t GlobalSystemInterrupt;
    uint16_t Flags;
} __attribute__((packed)) ACPI_MADT_INTERRUPT_SOURCE_OVERRIDE;

typedef struct {
    ACPI_MADT_ENTRY Entry;
    uint8_t ProcessorID;
    uint16_t Flags;
    uint8_t LINT;
} __attribute__((packed)) ACPI_MADT_LOCAL_APIC_NMI;

typedef struct {
    ACPI_MADT_ENTRY Entry;
    uint16_t Reserved;
    uint32_t X2APICID;
    uint32_t Flags;
    uint32_t ACPIProcessorUID;
} __attribute__((packed)) ACPI_MADT_LOCAL_X2APIC;

const ACPI_MADT *acpi_get_madt(void);

bool madt_has_legacy_pic(const ACPI_MADT *madt);

const ACPI_MADT_ENTRY *madt_first_entry(const ACPI_MADT *madt);
const ACPI_MADT_ENTRY *madt_next_entry(const ACPI_MADT *madt, const ACPI_MADT_ENTRY *cur);

void madt_parse(const ACPI_MADT *madt);

#define MADT_MAX_CPUS 64
#define MADT_MAX_IOAPICS 8
#define MADT_MAX_ISOS 16

int madt_get_cpu_count(void);
const ACPI_MADT_LOCAL_APIC *madt_get_cpu(int index);

int madt_get_ioapic_count(void);
const ACPI_MADT_IO_APIC *madt_get_ioapic(int index);

int madt_get_iso_count(void);
const ACPI_MADT_INTERRUPT_SOURCE_OVERRIDE *madt_get_iso(int index);

uint32_t madt_remap_isa_irq(uint8_t isa_irq);

#endif
