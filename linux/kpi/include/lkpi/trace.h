// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef _LKPI_TRACE_H
#define _LKPI_TRACE_H

#define tracepoint_string(str) str
#define PARAMS(args...) args
#define TP_PROTO(args...) args
#define TP_ARGS(args...) args
#define TP_CONDITION(args...) args
#define TP_STRUCT__entry(args...) args
#define TP_fast_assign(args...) args
#define TP_printk(fmt, args...) fmt
#define __LKPI_TRACE_STUBS(name, proto) \
	static inline void trace_##name(proto) { } \
	static inline bool trace_##name##_enabled(void) { return false; } \
	static inline int register_trace_##name(void *probe, void *data) { return 0; } \
	static inline int unregister_trace_##name(void *probe, void *data) { return 0; }
#define TRACE_EVENT(name, proto, args, tstruct, assign, print) __LKPI_TRACE_STUBS(name, PARAMS(proto))
#define TRACE_EVENT_CONDITION(name, proto, args, cond, tstruct, assign, print) __LKPI_TRACE_STUBS(name, PARAMS(proto))
#define TRACE_EVENT_FN(name, proto, args, tstruct, assign, print, reg, unreg) __LKPI_TRACE_STUBS(name, PARAMS(proto))
#define DECLARE_EVENT_CLASS(name, proto, args, tstruct, assign, print)
#define DEFINE_EVENT(template, name, proto, args) __LKPI_TRACE_STUBS(name, PARAMS(proto))
#define DEFINE_EVENT_CONDITION(template, name, proto, args, cond) __LKPI_TRACE_STUBS(name, PARAMS(proto))
#define DEFINE_EVENT_PRINT(template, name, proto, args, print) __LKPI_TRACE_STUBS(name, PARAMS(proto))
#define DECLARE_TRACE(name, proto, args) __LKPI_TRACE_STUBS(name, PARAMS(proto))
#define DECLARE_TRACE_CONDITION(name, proto, args, cond) __LKPI_TRACE_STUBS(name, PARAMS(proto))
#define DEFINE_TRACE(name, proto, args)
#define TRACE_DEFINE_ENUM(x)
#define TRACE_DEFINE_SIZEOF(x)
#define tracepoint_enabled(tp) false
#define DECLARE_TRACEPOINT(tp)
#define trace_event_lkpi 0

#endif
