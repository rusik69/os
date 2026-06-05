/*
 * kernel_pch.h — Precompiled header for kernel build speedup (Item 258)
 *
 * This header is precompiled via `-x c-header` and used automatically by
 * GCC when compiling any kernel .c file that includes any of the listed
 * headers.  It is included implicitly via `-include kernel_pch.h` in CFLAGS.
 *
 * Include here only headers that are:
 *   (a) very commonly used across all kernel subsystems, and
 *   (b) relatively stable (rarely modified).
 *
 * Adding too many headers makes the PCH large and invalidates it often,
 * reducing the benefit.  The current selection covers ~90% of translation
 * units with basic type definitions, memory allocator APIs, string/print
 * utilities, and common structure declarations.
 */
#ifndef _KERNEL_PCH_H
#define _KERNEL_PCH_H

/* ── Core type system (tiny, extremely stable) ───────────────────── */
#include "types.h"
#include "errno.h"

/* ── Standard library equivalents (used by virtually every file) ─── */
#include "string.h"
#include "printf.h"

/* ── Memory management (PMM + VMM + heap — widely referenced) ────── */
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "slab.h"

/* ── Process / scheduler types (embedded in many structs) ────────── */
#include "process.h"
#include "scheduler.h"

/* ── Synchronisation primitives ──────────────────────────────────── */
#include "spinlock.h"
#include "mutex.h"

/* ── Module export infrastructure ────────────────────────────────── */
#include "export.h"

#endif /* _KERNEL_PCH_H */
