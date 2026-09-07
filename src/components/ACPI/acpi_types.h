#ifndef ACPI_TYPES_H
#define ACPI_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef struct
{
    char Signature[4];
    uint32_t Length;
    uint8_t Revision;
    uint8_t Checksum;

    char OEMID[6];
    char OEMTableID[8];

    uint32_t OEMRevision;
    uint32_t CreatorID;
    uint32_t CreatorRevision;
} __attribute__((packed)) ACPI_SDT_HEADER;

static inline int acpi_signature_matches(const char *sig, const char ref[4]) {
    return sig[0] == ref[0] && sig[1] == ref[1] && sig[2] == ref[2] && sig[3] == ref[3];
}

#define ACPI_ADDRESS_SPACE_MEMORY 0x00
#define ACPI_ADDRESS_SPACE_IO 0x01
#define ACPI_ADDRESS_SPACE_PCI_CONFIG 0x02
#define ACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER 0x03
#define ACPI_ADDRESS_SPACE_SMBUS 0x04
#define ACPI_ADDRESS_SPACE_PLATFORM_COMM 0x0A
#define ACPI_ADDRESS_SPACE_FUNCTIONAL_FIXED 0x7F

typedef struct
{
    uint8_t AddressSpaceID;
    uint8_t RegisterBitWidth;
    uint8_t RegisterBitOffset;
    uint8_t AccessSize;
    uint64_t Address;
} __attribute__((packed)) ACPI_GAS;

#endif
