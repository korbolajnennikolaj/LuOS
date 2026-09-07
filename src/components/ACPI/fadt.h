#ifndef FADT_H
#define FADT_H

#include "acpi_types.h"

#include <stdbool.h>

typedef struct
{
    ACPI_SDT_HEADER Header;

    uint32_t FirmwareCtrl;
    uint32_t Dsdt;

    uint8_t Reserved;

    uint8_t PreferredPMProfile;

    uint16_t SCIInterrupt;

    uint32_t SMICommandPort;

    uint8_t AcpiEnable;
    uint8_t AcpiDisable;
    uint8_t S4BiosReq;
    uint8_t PStateControl;

    uint32_t PM1aEventBlock;
    uint32_t PM1bEventBlock;
    uint32_t PM1aControlBlock;
    uint32_t PM1bControlBlock;
    uint32_t PM2ControlBlock;
    uint32_t PMTimerBlock;

    uint32_t GPE0Block;
    uint32_t GPE1Block;

    uint8_t PM1EventLength;
    uint8_t PM1ControlLength;
    uint8_t PM2ControlLength;
    uint8_t PMTimerLength;
    uint8_t GPE0Length;
    uint8_t GPE1Length;
    uint8_t GPE1Base;
    uint8_t CStateControl;

    uint16_t WorstC2Latency;
    uint16_t WorstC3Latency;

    uint16_t FlushSize;
    uint16_t FlushStride;

    uint8_t DutyOffset;
    uint8_t DutyWidth;

    uint8_t DayAlarm;
    uint8_t MonthAlarm;
    uint8_t Century;

    uint16_t BootArchitectureFlags;

    uint8_t Reserved2;

    uint32_t Flags;

    ACPI_GAS ResetReg;
    uint8_t ResetValue;

    uint16_t ArmBootArch;
    uint8_t FadtMinorVersion;

    uint64_t X_FirmwareCtrl;
    uint64_t X_Dsdt;

    ACPI_GAS X_PM1aEventBlock;
    ACPI_GAS X_PM1bEventBlock;
    ACPI_GAS X_PM1aControlBlock;
    ACPI_GAS X_PM1bControlBlock;
    ACPI_GAS X_PM2ControlBlock;
    ACPI_GAS X_PMTimerBlock;

    ACPI_GAS X_GPE0Block;
    ACPI_GAS X_GPE1Block;

    ACPI_GAS SleepControlReg;
    ACPI_GAS SleepStatusReg;

    uint64_t HypervisorVendorID;

} __attribute__((packed)) ACPI_FADT;

#define ACPI_FADT_WBINVD (1u << 0)
#define ACPI_FADT_PROC_C1 (1u << 2)
#define ACPI_FADT_PWR_BUTTON (1u << 4)
#define ACPI_FADT_SLP_BUTTON (1u << 5)
#define ACPI_FADT_FIX_RTC (1u << 6)
#define ACPI_FADT_TMR_VAL_EXT (1u << 8)
#define ACPI_FADT_RESET_REG_SUP (1u << 10)
#define ACPI_FADT_HW_REDUCED_ACPI (1u << 20)

#define ACPI_PM1_CNT_SLP_EN (1u << 13)
#define ACPI_PM1_CNT_SCI_EN (1u << 0)
#define ACPI_PM1_CNT_SLP_TYP_SHIFT 10

const ACPI_FADT *acpi_get_fadt(void);

uint64_t fadt_get_dsdt_address(const ACPI_FADT *fadt);

bool fadt_has_fixed_hardware(const ACPI_FADT *fadt);

bool fadt_read_pm_timer(const ACPI_FADT *fadt, uint32_t *out_value);

void fadt_reset_system(const ACPI_FADT *fadt);

#endif
