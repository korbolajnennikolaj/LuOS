// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef KERNEL_SCHEDULER_FPU_H
#define KERNEL_SCHEDULER_FPU_H

#include <stdint.h>

extern uint64_t fpu_save_size;
extern uint8_t fpu_use_xsave;

void fpu_init_cpu(int cpu);
uint64_t fpu_xcr0(void);

#endif
