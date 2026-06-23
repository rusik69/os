# Coding Style Guide

This document defines the coding conventions for the OS kernel project.
All new code should follow these guidelines. Exceptions for existing code
are noted where applicable.

## Naming Conventions

### Identifiers

| Category | Convention | Example |
|----------|-----------|---------|
| Functions | `snake_case` | `pmm_alloc_frame()`, `scheduler_tick()` |
| Global variables | `snake_case` | `process_table`, `current_process` |
| Local variables | `snake_case` | `total_pages`, `entry_idx` |
| Type names (typedef) | `snake_case` (usually suffixed `_t`) | `size_t`, `pid_t` |
| Struct names | `snake_case` | `struct process`, `struct cpu_context` |
| Macros / Constants | `UPPER_SNAKE_CASE` | `PROCESS_MAX`, `PAGE_SIZE` |
| Enum values | `UPPER_SNAKE_CASE` | `PROCESS_READY`, `PROCESS_BLOCKED` |
| File-scope statics | `snake_case` with `s_` prefix (optional) | `s_initialized` |

### File Naming

- Source files: `snake_case.c` (e.g., `pmm.c`, `sched_deadline.c`)
- Header files: `snake_case.h` (e.g., `process.h`, `vmm.h`)
- Assembly files: `snake_case.asm` (e.g., `switch.asm`, `boot.asm`)
- One major subsystem per file; closely related helpers may share a file

### Prefix Conventions

- **Subsystem prefix:** Functions use a subsystem prefix for clarity:
  - `pmm_*` — Physical Memory Manager
  - `vmm_*` — Virtual Memory Manager
  - `sched_*` — Scheduler
  - `process_*` — Process management
  - `signal_*` — Signal delivery
  - `vfs_*` — Virtual filesystem layer
  - `net_*` — Networking (core IP layer)
  - `tcp_*` — TCP transport
  - `udp_*` — UDP transport
  - `acpi_*` — ACPI table parsing and AML
  - `pci_*` — PCI/PCIe bus
  - `usb_*` — USB subsystem
  - `bpf_*` — eBPF subsystem
  - `kvm_*` — KVM virtualization
  - `blk_*` — Block device layer
  - `dm_*` — Device mapper
- **Driver functions:** Use the device name as prefix (e.g., `e1000_*`, `virtio_blk_*`)

### Header Guards

```c
#ifndef FILENAME_H
#define FILENAME_H

/* ... */

#endif /* FILENAME_H */
```

Use the uppercase filename with underscores, no path components, no trailing
`_H` suffix (just `_H` is preferred over `_H_`).

## Indentation and Formatting

- **Indentation:** Tabs, width 4 spaces per tab. Use actual tab characters.
- **Line length:** Maximum 100 characters. Break lines before 100 cols when
  possible.
- **Braces:** K&R style (opening brace on same line as statement):

```c
/* Correct */
if (condition) {
    do_something();
} else {
    do_other();
}

/* Functions: opening brace on next line */
void my_function(void)
{
    /* body */
}
```

- **Spaces:**
  - One space between `if`/`for`/`while` and opening parenthesis: `if (x)`
  - No space between function name and `(`: `my_func(42)`
  - No space inside parentheses: `(x + 1)`, not `( x + 1 )`
  - Space after `,` in function arguments: `func(a, b, c)`
  - Space around binary operators: `x + y`, `a == b`, `p = &c`
  - No space around unary operators: `!flag`, `*ptr`, `&addr`
  - Pointer `*` attaches to the variable name: `int *ptr`, not `int* ptr`
- **Switch:** Indent case labels at same level as switch:

```c
switch (state) {
case PROCESS_READY:
    enqueue(proc);
    break;
default:
    break;
}
```

## Comment Style

### File Header

Every source file should begin with a brief description:

```c
/*
 * pmm.c — Physical Memory Manager
 *
 * Bitmap-based allocator with per-CPU hot caches. Manages
 * physical page frames using a bitmap where each bit represents
 * one 4 KB page.
 */
```

### Function Comments

Public / non-trivial functions should have a comment block:

```c
/*
 * pmm_alloc_frame — Allocate a single physical page frame
 * @return: Physical address of the frame, or 0 on failure
 *
 * Allocates from the per-CPU hot cache first (lockless fast path),
 * then falls back to scanning the bitmap with lock held.
 * Never panics — returns 0 if no memory is available.
 */
uint64_t pmm_alloc_frame(void)
```

For simpler functions, a single-line comment above the function is sufficient:

```c
/* Allocate a zeroed page frame */
uint64_t pmm_alloc_zeroed_frame(void)
```

### Inline Comments

- Use `/* ... */` for block comments. Do not use `//` style comments.
- Place comments on their own line, not at end of code lines.
- Explain *why*, not *what* — the code already says what it does.

### Section Dividers

Use `──` dividers for major sections within a file:

```c
/* ── Constants ─────────────────────────────────────────────────────────── */

/* ── Internal helpers ──────────────────────────────────────────────────── */

/* ── Public API ──────────────────────────────────────────────────────────── */
```

## Error Handling

### Return Values

- **Functions that can fail:** Return a negative errno value on error,
  0 or a positive result on success.
- **Allocation functions:** Return NULL on failure (never panic). Callers
  must check the return value.
- **File I/O:** Return number of bytes transferred on success, negative
  errno on error. Use standard errno values: `-EINVAL`, `-ENOMEM`, `-EIO`,
  `-ENOENT`, `-EACCES`, `-EBUSY`, etc.

```c
int my_function(void) {
    void *buf = kmalloc(256);
    if (!buf)
        return -ENOMEM;

    int ret = do_work(buf);
    if (ret < 0)
        goto err;
    kfree(buf);
    return 0;

err:
    kfree(buf);
    return ret;
}
```

### NULL Checks

Always check pointer returns from allocation functions:

```c
struct process *proc = process_create();
if (!proc)
    return -ENOMEM;
```

### Validation

- Validate all userspace-provided pointers, sizes, and offsets.
- Use `copy_from_user()` / `copy_to_user()` for userspace memory access.
- Check array bounds and integer overflow (use `kmalloc_array()` for
  multiply-based sizes).

## Data Structures

### Struct Initialization

Use designated initializers for clarity:

```c
struct process proc = {
    .pid = 42,
    .state = PROCESS_READY,
    .priority = 120,
};
```

### Dynamic Allocation

- Prefer `kmalloc()` / `kfree()` for variable-size allocations.
- Prefer `kmem_cache_alloc()` / `kmem_cache_free()` for fixed-size objects.
- Use `kmalloc_array()` / `krealloc_array()` for array allocations to
  detect integer overflow.
- Always pair allocation and deallocation at the same level.

## Function Organization

### Length

- Functions should be short and focused (typically < 50 lines).
- Complex functions may be longer but should be rare. Consider splitting
  into helper functions.

### Static Functions

- File-internal functions should be `static`.
- Non-static functions should have prototypes in a header file.

### Parameter Order

- Input parameters first, output parameters (pointers) last.
- Object/context pointer first where applicable (OOP-like style).

## Preprocessor

### Macros

- Use macros sparingly. Prefer inline functions or `static const` variables.
- Macro names in UPPER_SNAKE_CASE.
- Multi-statement macros should use `do { } while (0)`:

```c
#define DBG_PRINT(fmt, ...) \
    do { \
        if (debug_enabled) \
            printf(fmt, ##__VA_ARGS__); \
    } while (0)
```

### Conditional Compilation

- Prefer `if (IS_ENABLED(CONFIG_...))` over `#ifdef` where possible.
- Use `#ifdef` / `#ifndef` only when the code would not compile without it
  (e.g., platform-specific assembly).
- Keep `#ifdef` blocks as short as possible.

## Include Order

Headers should be included in the following order:

1. Primary header for the source file (e.g., `pmm.h` for `pmm.c`)
2. Kernel internal headers
3. Library headers
4. Architecture-specific headers (if applicable)

```c
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"
#include "smp.h"
```

Use quotes (`"..."`) for kernel headers, angle brackets (`<...>`) for
system/library headers.

## Locking

- Use `spinlock_irqsave_acquire()` / `spinlock_irqsave_release()` in
  interrupt context or when shared with interrupt handlers.
- Use `spinlock_acquire()` / `spinlock_release()` in process context
  with interrupts already disabled.
- Use `rwlock_*` for read-mostly data with occasional writes.
- Use `mutex_lock()` / `mutex_unlock()` for long-held locks.
- Document which lock protects which data structures.
- Avoid nested locks where possible; document lock ordering when required.

## Testing

- Add in-kernel tests to `src/test/test.c` for new functionality.
- For new subsystems, consider adding KUnit tests in
  `src/test/kunit_tests.c`.
- Ensure all existing tests pass before submitting changes.
- Run `make build-strict` to catch warnings and static analysis issues.

## Portability

- Use `uint64_t`, `uint32_t`, `uint16_t`, `uint8_t` from `<types.h>` for
  fixed-width types. Prefer `int`/`unsigned int` for general-purpose locals.
- Use `size_t` for sizes and counts.
- Use `NULL` for null pointers, not `(void *)0` or `0`.
- Avoid architecture-specific assumptions unless guarded by `#ifdef`.
- Use `PAGE_SIZE` constant instead of literal `4096`.
