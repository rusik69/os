/*
 * coredump_core.c — Core dump handler registration and dispatch.
 *
 * Provides a function-pointer indirection so that the core dump handler
 * can live in a loadable kernel module (coredump.ko) without the kernel
 * core needing a direct link-time dependency.
 *
 * The built-in coredump (drivers/coredump.c) registers on boot via the
 * same path, maintaining a single dispatch interface.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "coredump_core.h"
#include "printf.h"
#include "export.h"

/* ── Registered handler (initially NULL — no coredump support) ─── */

static void (*coredump_handler)(uint32_t pid) = NULL;

/* ── Public API ────────────────────────────────────────────────── */

void coredump_trigger(uint32_t pid)
{
    if (coredump_handler)
        coredump_handler(pid);
}
EXPORT_SYMBOL(coredump_trigger);

int coredump_register_handler(void (*handler)(uint32_t pid))
{
    if (!handler)
        return -1;

    if (coredump_handler) {
        kprintf("[coredump_core] handler already registered (0x%llx), "
                "rejecting 0x%llx\n",
                (unsigned long long)(uintptr_t)coredump_handler,
                (unsigned long long)(uintptr_t)handler);
        return -1; /* -EBUSY */
    }

    coredump_handler = handler;
    kprintf("[coredump_core] handler registered at 0x%llx\n",
            (unsigned long long)(uintptr_t)handler);
    return 0;
}
EXPORT_SYMBOL(coredump_register_handler);

void coredump_unregister_handler(void)
{
    if (coredump_handler) {
        kprintf("[coredump_core] handler 0x%llx unregistered\n",
                (unsigned long long)(uintptr_t)coredump_handler);
        coredump_handler = NULL;
    }
}
EXPORT_SYMBOL(coredump_unregister_handler);
