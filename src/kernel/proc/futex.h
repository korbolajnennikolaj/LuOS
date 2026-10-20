// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef FUTEX_H
#define FUTEX_H

#include "kernel/proc/process.h"

#include <stdint.h>

int futex_wait(struct process *p, uint64_t uaddr, uint32_t val, uint64_t timeout_ms, uint32_t bitset);
int futex_wake(struct process *p, uint64_t uaddr, int count, uint32_t bitset);
int futex_requeue(struct process *p, uint64_t uaddr, int wake, int requeue, uint64_t uaddr2, int cmp,
                  uint32_t cmpval);

#endif
