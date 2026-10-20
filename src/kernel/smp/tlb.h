// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef TLB_H
#define TLB_H

#include <stdint.h>

void tlb_init(void);
void tlb_cpu_online(void);

void tlb_shootdown(uint64_t start, uint64_t size);
void tlb_shootdown_all(void);

int tlb_online_cpus(void);
uint64_t tlb_shootdown_count(void);
int tlb_cpu_lapic_ids(uint8_t *out, int max);

#endif
