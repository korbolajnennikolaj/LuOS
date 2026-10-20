// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "intel_legacy.h"
#include "gpu_core.h"
#include "components/logger.h"
#include "components/pci.h"
#include <stdio.h>

static const intel_legacy_id intel_legacy_ids[] = {
    {0x7121, 1, "i810", "i810 Graphics"},
    {0x7123, 1, "i810-DC100", "i810 Graphics"},
    {0x7125, 1, "i810e", "i810e Graphics"},
    {0x1132, 1, "i815", "i815 Graphics"},
    {0x3577, 2, "i830M", "830M"},
    {0x2562, 2, "i845G", "845G"},
    {0x3582, 2, "i855GM", "855GM"},
    {0x358e, 2, "i855GM", "855GM"},
    {0x2572, 2, "i865G", "865G"},
    {0x2582, 3, "i915G", "GMA 900"},
    {0x258a, 3, "E7221", "GMA 900"},
    {0x2592, 3, "i915GM", "GMA 900"},
    {0x2772, 3, "i945G", "GMA 950"},
    {0x27a2, 3, "i945GM", "GMA 950"},
    {0x27ae, 3, "i945GME", "GMA 950"},
    {0x29b2, 3, "Q35", "GMA 3100"},
    {0x29c2, 3, "G33", "GMA 3100"},
    {0x29d2, 3, "Q33", "GMA 3100"},
    {0xa001, 3, "Pineview G", "GMA 3150"},
    {0xa011, 3, "Pineview M", "GMA 3150"},
    {0x2972, 4, "i946GZ", "GMA 3000"},
    {0x2982, 4, "iG35", "GMA X3500"},
    {0x2992, 4, "Q965", "GMA 3000"},
    {0x29a2, 4, "i965G", "GMA X3000"},
    {0x2a02, 4, "i965GM", "GMA X3000"},
    {0x2a12, 4, "i965GME", "GMA X3000"},
    {0x2e02, 5, "B43", "GMA X4500"},
    {0x2e12, 5, "B43", "GMA X4500"},
    {0x2e22, 5, "G41", "GMA X4500"},
    {0x2e32, 5, "G45", "GMA X4500HD"},
    {0x2e42, 5, "B43", "GMA X4500"},
    {0x2e92, 5, "B43", "GMA X4500"},
    {0x2a42, 5, "GM45", "GMA X4500MHD"},
    {0x0042, 5, "Ironlake", "HD Graphics"},
    {0x0046, 5, "Ironlake", "HD Graphics"},
    {0x0102, 6, "SandyBridge", "HD Graphics 2000"},
    {0x0112, 6, "SandyBridge", "HD Graphics 3000"},
    {0x0126, 6, "SandyBridge", "HD Graphics 3000"},
    {0x010a, 6, "SandyBridge", "HD Graphics 2000"},
    {0x0152, 6, "IvyBridge", "HD Graphics 2500"},
    {0x0162, 6, "IvyBridge", "HD Graphics 4000"},
    {0x0166, 6, "IvyBridge", "HD Graphics 4000"},
    {0x015a, 6, "IvyBridge", "HD Graphics 2500"},
    {0x0412, 7, "Haswell", "HD Graphics 4600"},
    {0x0416, 7, "Haswell", "HD Graphics 4600"},
    {0x0402, 7, "Haswell", "HD Graphics"},
    {0x0406, 7, "Haswell", "HD Graphics"},
    {0x0a16, 7, "Haswell", "HD Graphics 4400"},
    {0x0c02, 7, "Haswell", "HD Graphics"},
    {0x0d12, 7, "Haswell", "HD Graphics 5000"},
    {0x1606, 8, "Broadwell", "HD Graphics"},
    {0x1616, 8, "Broadwell", "HD Graphics 5500"},
    {0x1626, 8, "Broadwell", "HD Graphics 6000"},
    {0x162b, 8, "Broadwell", "Iris Pro 6200"},
};

const intel_legacy_id *intel_legacy_lookup(uint16_t device) {
    for (size_t i = 0; i < sizeof(intel_legacy_ids) / sizeof(intel_legacy_ids[0]); i++)
        if (intel_legacy_ids[i].device == device) return &intel_legacy_ids[i];
    return NULL;
}

int intel_legacy_scan(intel_legacy_report_fn report, void *user) {
    int found = 0;
    for (int i = 0; i < pci_get_device_count(); i++) {
        pci_device *pdev = pci_get_device(i);
        if (!pdev || pdev->vendor_id != 0x8086 || pdev->class_code != 0x03) continue;
        const intel_legacy_id *id = intel_legacy_lookup(pdev->device_id);
        if (!id) continue;
        char bdf[16];
        snprintf(bdf, sizeof(bdf), "%02x:%02x.%x", pdev->bus, pdev->slot, pdev->func);
        if (report) report(user, bdf, pdev, id);
        found++;
    }
    return found;
}

int intel_legacy_bind_all(void) {
    gpu_core_refresh();
    int bound = 0;
    for (int i = 0; i < gpu_get_device_count(); i++) {
        struct gpu_device *d = gpu_get_device(i);
        if (!d || d->vendor != 0x8086) continue;
        const intel_legacy_id *id = intel_legacy_lookup(d->device);
        if (!id) continue;
        if (d->flags & GPU_DEV_FLAG_BOUND) continue;
        if (gpu_bind_device(i) == GPU_OK) bound++;
    }
    return bound;
}

static void intel_legacy_init(void) {
    int n = intel_legacy_scan(NULL, NULL);
    if (n) LOG_INFO("intel-legacy: %d old Intel iGPU(s) present (gpu-intel binds them)", n);
}

struct driver *return_meta_intel_legacy_driver(void) {
    static struct driver meta = {
        .name = "Intel Legacy GPU Driver",
        .type = GPU_DRIVER,
        .sub_type = GPU_INTEL_LEGACY_SLOT,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { NULL },
        .dependency_count = 0,
        .self = NULL,
        .init = intel_legacy_init,
    };
    return &meta;
}
