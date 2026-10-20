// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef PROC_ELF_H
#define PROC_ELF_H

#include "kernel/proc/process.h"

#include <stdint.h>

int elf_exec(struct process *p, const char *path, int argc, const char *const *argv, int envc,
             const char *const *envp, uint64_t *entry, uint64_t *sp);

#endif
