// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef PROC_SYSCALL_H
#define PROC_SYSCALL_H

#include "components/Interruptions/isr.h"

#include <stdint.h>

void syscall_handler(struct registers *regs, void *fx);

long sys_fs_dispatch(struct registers *r, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5, int *handled);

long proc_clone_thread(struct registers *regs, uint64_t flags, uint64_t newsp, uint64_t ptid, uint64_t ctid,
                       uint64_t tls);

uint64_t sys_monotonic_ns(void);
uint64_t sys_realtime_ns(void);
void vfs_random_bytes(void *buf, uint64_t n);

uint64_t syscall_count(void);
void syscall_set_trace(int on);
int syscall_get_trace(void);

#endif
