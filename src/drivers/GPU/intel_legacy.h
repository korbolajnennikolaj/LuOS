// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef INTEL_LEGACY_H
#define INTEL_LEGACY_H
#include "components/drivers.h"
#include <stdint.h>
#define GPU_INTEL_LEGACY_SLOT 62
typedef struct intel_legacy_id {
    uint16_t device;
    uint8_t gen;
    const char *chip;
    const char *marketing;
} intel_legacy_id;

const intel_legacy_id *intel_legacy_lookup(uint16_t device);
typedef void (*intel_legacy_report_fn)(void *user, const char *bdf,
                                       const pci_device *pdev, const intel_legacy_id *id);
int intel_legacy_scan(intel_legacy_report_fn report, void *user);
int intel_legacy_bind_all(void);
struct driver *return_meta_intel_legacy_driver(void);
#endif
