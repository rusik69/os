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
#include "coredump_core.h"
#include "types.h"
#include "printf.h"
#include "export.h"

/* ── Registered handler (initially NULL — no coredump support) ─── */

static coredump_handler_fn coredump_handler = NULL;

/* ── Public API ────────────────────────────────────────────────── */

/**
 * coredump_trigger - Dispatch a core dump event to the registered handler
 * @pid: PID of the process that triggered the core dump
 * @signo: Signal number that caused the core dump
 *
 * If a coredump handler has been registered via coredump_register_handler(),
 * this function forwards the event to it.  Otherwise the event is silently
 * ignored.
 */
void coredump_trigger(uint32_t pid, int signo)
{
    if (coredump_handler)
        coredump_handler(pid, signo);
}
EXPORT_SYMBOL(coredump_trigger);

/**
 * coredump_register_handler - Register a core dump handler function
 * @handler: Pointer to the handler function to register
 *
 * Registers a callback that will be invoked on core dump events.  Only
 * one handler may be registered at a time; subsequent registration
 * attempts are rejected.
 *
 * Return: 0 on success, -ENOMEM if @handler is NULL, -EBUSY if a
 *         handler is already registered
 */
int coredump_register_handler(coredump_handler_fn handler)
{
    if (!handler)
        return -ENOMEM;

    if (coredump_handler) {
        kprintf("[COREDUMP_CORE] handler already registered (0x%llx), "
                "rejecting 0x%llx\n",
                (unsigned long long)(uintptr_t)coredump_handler,
                (unsigned long long)(uintptr_t)handler);
        return -EBUSY; /* -EBUSY */
    }

    coredump_handler = handler;
    kprintf("[COREDUMP_CORE] handler registered at 0x%llx\n",
            (unsigned long long)(uintptr_t)handler);
    return 0;
}
EXPORT_SYMBOL(coredump_register_handler);

/**
 * coredump_unregister_handler - Unregister the current core dump handler
 *
 * Removes the previously registered coredump handler.  After this call,
 * coredump_trigger() will silently ignore events until a new handler
 * is registered.
 */
void coredump_unregister_handler(void)
{
    if (coredump_handler) {
        kprintf("[COREDUMP_CORE] handler 0x%llx unregistered\n",
                (unsigned long long)(uintptr_t)coredump_handler);
        coredump_handler = NULL;
    }
}
EXPORT_SYMBOL(coredump_unregister_handler);

/* ── Stub: coredump_core_init ─────────────────────────────── */
static int coredump_core_init(void)
{
    kprintf("[COREDUMP] coredump_core_init: not yet implemented\n");
    return 0;
}
/* ── Stub: coredump_core_write ─────────────────────────────── */
static int coredump_core_write(const void *buf, size_t count)
{
    (void)buf;
    (void)count;
    kprintf("[COREDUMP] coredump_core_write: not yet implemented\n");
    return 0;
}
/* ── Stub: coredump_core_read ─────────────────────────────── */
static int coredump_core_read(void *buf, size_t count)
{
    (void)buf;
    (void)count;
    kprintf("[COREDUMP] coredump_core_read: not yet implemented\n");
    return 0;
}
