// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef _LKPI_EXPORT_H
#define _LKPI_EXPORT_H
#define EXPORT_SYMBOL(sym)
#define EXPORT_SYMBOL_GPL(sym)
#define EXPORT_SYMBOL_NS(sym, ns)
#define EXPORT_SYMBOL_NS_GPL(sym, ns)
#define EXPORT_SYMBOL_IF_KUNIT(sym)
#define EXPORT_IF_CONFIG_KUNIT(sym)
#define VISIBLE_IF_KUNIT static
#define MODULE_IMPORT_NS(ns)
#define EXPORT_TRACEPOINT_SYMBOL(x)
#define EXPORT_TRACEPOINT_SYMBOL_GPL(x)
#define EXPORT_PER_CPU_SYMBOL(x)
#define EXPORT_PER_CPU_SYMBOL_GPL(x)
#endif
