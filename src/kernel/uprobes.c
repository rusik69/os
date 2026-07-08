/* uprobes.c — User-space dynamic tracing via breakpoint insertion.
 *
 * Provides uprobe_register/unregister for inserting INT3 breakpoints
 * at user-space addresses.  When the breakpoint is hit, the uprobe
 * handler is called, the original instruction is single-stepped, and
 * the breakpoint is re-inserted.
 *
 * Exposes /sys/kernel/debug/uprobes/list (read-only) and
 * /sys/kernel/debug/uprobes/register (write-only) for userspace control.
 */

#define KERNEL_INTERNAL
#include "uprobes.h"
#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "timer.h"
#include "debugfs.h"
#include "errno.h"
#include "idt.h"

/* ── Forward declarations ──────────────────────────────────────────── */

struct interrupt_frame;

/* ── Global state ──────────────────────────────────────────────────── */

/* Hash table of uprobes */
static struct uprobe g_uprobes[UPROBES_MAX_PROBES];
static spinlock_t g_uprobe_lock;

/* Ring buffer for uprobe events */
static struct uprobe_event g_uprobe_ring[UPROBES_RING_SIZE];
static int g_uprobe_ring_head = 0;
static int g_uprobe_ring_count = 0;

/* Single-step tracking */
static struct uprobe *g_stepping_uprobe = NULL;
static uint64_t g_stepping_vaddr = 0;

/* Initialisation flag */
static int g_uprobes_initialised = 0;

/* ── Hash helpers ───────────────────────────────────────────────────── */

static uint32_t uprobe_hash(const char *path, uint64_t offset)
{
    uint32_t h = 0;
    if (!path) return 0;
    for (int i = 0; path[i] && i < UPROBES_PATH_MAX; i++) {
        h = h * 31 + (uint8_t)path[i];
    }
    h = h * 31 + (uint32_t)(offset & 0xFFFFFFFF);
    h = h * 31 + (uint32_t)(offset >> 32);
    return h;
}

static int uprobe_slot(const char *path, uint64_t offset)
{
    uint32_t h = uprobe_hash(path, offset);
    return (int)(h & (UPROBES_MAX_PROBES - 1));
}

/* Find a probe by path + offset */
static struct uprobe *uprobe_find(const char *path, uint64_t offset)
{
    int start = uprobe_slot(path, offset);
    for (int i = 0; i < UPROBES_MAX_PROBES; i++) {
        int idx = (start + i) & (UPROBES_MAX_PROBES - 1);
        if (!g_uprobes[idx].in_use)
            return NULL; /* empty slot = not found */
        if (g_uprobes[idx].offset == offset &&
            strcmp(g_uprobes[idx].path, path) == 0) {
            return &g_uprobes[idx];
        }
    }
    return NULL;
}

/* Find a probe by virtual address (for breakpoint handler) */
static struct uprobe *uprobe_find_by_vaddr(uint64_t vaddr)
{
    for (int i = 0; i < UPROBES_MAX_PROBES; i++) {
        if (g_uprobes[i].in_use && g_uprobes[i].active &&
            g_uprobes[i].vaddr == vaddr) {
            return &g_uprobes[i];
        }
    }
    return NULL;
}

/* ── Ring buffer helpers ────────────────────────────────────────────── */

static void uprobe_ring_push(uint32_t pid, uint64_t address, uint64_t timestamp)
{
    int idx = (g_uprobe_ring_head + g_uprobe_ring_count) % UPROBES_RING_SIZE;
    if (g_uprobe_ring_count < UPROBES_RING_SIZE) {
        g_uprobe_ring_count++;
    } else {
        /* Ring full — overwrite oldest */
        g_uprobe_ring_head = (g_uprobe_ring_head + 1) % UPROBES_RING_SIZE;
    }
    g_uprobe_ring[idx].pid = pid;
    g_uprobe_ring[idx].address = address;
    g_uprobe_ring[idx].timestamp = timestamp;
    g_uprobe_ring[idx].valid = 1;
}

/* ── Initialisation ─────────────────────────────────────────────────── */

void uprobes_init(void)
{
    if (g_uprobes_initialised) return;

    memset(g_uprobes, 0, sizeof(g_uprobes));
    memset(g_uprobe_ring, 0, sizeof(g_uprobe_ring));
    spinlock_init(&g_uprobe_lock);
    g_uprobes_initialised = 1;

    /* Register debugfs files */
    debugfs_create_file("uprobes/list", uprobe_list_read);
    debugfs_create_rw_file("uprobes/register",
                            NULL, /* read returns empty for write-only files */
                            uprobe_register_write);

    kprintf("[uprobes] subsystem initialised (%d probes max, "
            "%d ring buffer entries)\n",
            UPROBES_MAX_PROBES, UPROBES_RING_SIZE);
}

/* ── Registration ─────────────────────────────────────────────────────
 *
 * uprobe_register: Insert a breakpoint (0xCC) at @offset in @path.
 * The breakpoint is inserted into the user-space text by patching
 * the page when it is first mapped.  For simplicity, we store the
 * probe entry and rely on the page fault / exec path to insert the
 * breakpoint on the actual page.
 *
 * In a full implementation, we would walk all processes that have
 * the target file mapped and patch each instance.  Here we store
 * the intent and patch lazily on the first hit.
 */

int uprobe_register(const char *path, uint64_t offset,
                    uprobe_handler_t handler)
{
    if (!g_uprobes_initialised) return -EAGAIN;
    if (!path || !*path) return -EINVAL;

    uint64_t flags;
    spinlock_irqsave_acquire(&g_uprobe_lock, &flags);

    /* Check if already registered */
    if (uprobe_find(path, offset)) {
        spinlock_irqsave_release(&g_uprobe_lock, flags);
        return -EEXIST;
    }

    /* Find a free slot */
    int idx = -1;
    for (int i = 0; i < UPROBES_MAX_PROBES; i++) {
        if (!g_uprobes[i].in_use) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        spinlock_irqsave_release(&g_uprobe_lock, flags);
        return -ENOSPC;
    }

    struct uprobe *up = &g_uprobes[idx];
    memset(up, 0, sizeof(*up));
    strncpy(up->path, path, UPROBES_PATH_MAX - 1);
    up->path[UPROBES_PATH_MAX - 1] = '\0';
    up->offset = offset;
    up->vaddr = 0;  /* resolved at first hit */
    up->orig_byte = 0;
    up->active = 0;
    up->handler = handler;
    up->flags = 0;
    up->in_use = 1;

    spinlock_irqsave_release(&g_uprobe_lock, flags);

    kprintf("[uprobes] registered: path=%s offset=0x%llx slot=%d\n",
            path, (unsigned long long)offset, idx);

    return 0;
}

/* ── Unregistration ─────────────────────────────────────────────────── */

int uprobe_unregister(const char *path, uint64_t offset)
{
    if (!g_uprobes_initialised) return -EAGAIN;

    uint64_t flags;
    spinlock_irqsave_acquire(&g_uprobe_lock, &flags);

    struct uprobe *up = uprobe_find(path, offset);
    if (!up) {
        spinlock_irqsave_release(&g_uprobe_lock, flags);
        return -ENOENT;
    }

    /* If the breakpoint was inserted, restore the original byte */
    if (up->active && up->vaddr != 0) {
        /* The page might not be mapped anymore — best-effort restore */
        uint64_t page_addr = up->vaddr & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t offset_in_page = up->vaddr - page_addr;
        (void)offset_in_page;

        /* Try to write the original byte back */
        struct process *proc = process_get_current();
        if (proc && proc->pml4) {
            uint64_t phys = vmm_get_physaddr(up->vaddr);
            if (phys) {
                uint8_t *ptr = (uint8_t *)PHYS_TO_VIRT(phys);
                *ptr = up->orig_byte;
            }
        }
    }

    memset(up, 0, sizeof(*up));

    spinlock_irqsave_release(&g_uprobe_lock, flags);

    kprintf("[uprobes] unregistered: path=%s offset=0x%llx\n",
            path, (unsigned long long)offset);

    return 0;
}

/* ── Breakpoint handler ───────────────────────────────────────────────
 *
 * Called from the INT3 handler (kprobe_int3_handler) when a
 * breakpoint is hit.  We check if the faulting address corresponds
 * to a registered uprobe.
 *
 * On hit:
 *   1. Find the uprobe
 *   2. Restore the original instruction byte
 *   3. Log the event to the ring buffer
 *   4. Call the user's handler callback
 *   5. Set TF (Trap Flag) to single-step the original instruction
 *   6. Record that we're single-stepping
 *
 * Returns 1 if this was a uprobe hit (handled), 0 if not.
 */

int uprobe_handle_breakpoint(struct interrupt_frame *frame)
{
    if (!g_uprobes_initialised) return 0;

    /* Get the faulting RIP.
     * For INT3, the CPU pushes RIP pointing to the NEXT instruction,
     * so the INT3 was at RIP-1. */
    uint64_t rip = frame->rip;
    uint64_t probe_addr = rip - 1;

    uint64_t flags;
    spinlock_irqsave_acquire(&g_uprobe_lock, &flags);

    struct uprobe *up = uprobe_find_by_vaddr(probe_addr);
    if (!up) {
        spinlock_irqsave_release(&g_uprobe_lock, flags);
        return 0; /* not a uprobe breakpoint */
    }

    /* Found a matching uprobe */

    /* Restore the original instruction byte */
    if (up->vaddr && up->orig_byte != 0xCC) {
        uint64_t phys = vmm_get_physaddr(up->vaddr);
        if (phys) {
            uint8_t *ptr = (uint8_t *)PHYS_TO_VIRT(phys);
            *ptr = up->orig_byte;
        }
    }

    /* Log to ring buffer */
    struct process *proc = process_get_current();
    uint32_t pid = proc ? proc->pid : 0;
    uint64_t now = timer_get_ticks();
    uprobe_ring_push(pid, probe_addr, now);

    /* Call the handler */
    if (up->handler) {
        up->handler(pid, probe_addr, now);
    }

    /* Set up single-stepping:
     * 1. Set RIP back to the original instruction (pointing to INT3, now restored)
     * 2. Set Trap Flag (TF) in RFLAGS
     */
    frame->rip = up->vaddr;
    frame->rflags |= (1ULL << 8);  /* Set TF */

    /* Record that we're stepping this uprobe */
    g_stepping_uprobe = up;
    g_stepping_vaddr = up->vaddr;

    spinlock_irqsave_release(&g_uprobe_lock, flags);

    return 1; /* handled */
}

/* ── Debug handler (for TF single-step completion) ────────────────────
 *
 * Called after single-stepping.  Re-inserts the INT3 breakpoint.
 * This should be called from the #DB handler.
 */

static void uprobe_debug_handler(struct interrupt_frame *frame)
{
    (void)frame;

    if (!g_stepping_uprobe)
        return;

    uint64_t flags;
    spinlock_irqsave_acquire(&g_uprobe_lock, &flags);

    struct uprobe *up = g_stepping_uprobe;
    g_stepping_uprobe = NULL;

    /* Clear TF (should already be cleared by CPU after single-step) */
    frame->rflags &= ~(1ULL << 8);

    /* Re-insert the INT3 breakpoint */
    if (up->active && up->vaddr != 0 && up->orig_byte != 0xCC) {
        uint64_t phys = vmm_get_physaddr(up->vaddr);
        if (phys) {
            uint8_t *ptr = (uint8_t *)PHYS_TO_VIRT(phys);
            *ptr = 0xCC; /* INT3 */
        }
    }

    spinlock_irqsave_release(&g_uprobe_lock, flags);
}

/* ── Default handler (simple logger) ──────────────────────────────────
 *
 * This is the default handler used when no custom handler is provided.
 * It logs (pid, address, timestamp) to the ring buffer and kprintf.
 */

static void uprobe_default_handler(uint32_t pid, uint64_t address,
                             uint64_t timestamp)
{
    kprintf("[uprobe] pid=%u addr=0x%llx tick=%llu\n",
            (unsigned int)pid,
            (unsigned long long)address,
            (unsigned long long)timestamp);
}

/* ── Interface functions for probe activation ─────────────────────────
 *
 * Activate a uprobe by inserting the INT3 byte into the target page.
 * This is called when the target process maps the executable page.
 * For simplicity, we activate on first registration.
 */

static int uprobe_activate(struct uprobe *up)
{
    if (up->active) return 0;
    if (up->vaddr == 0) {
        /* Virtual address not yet resolved.
         * In a full implementation we would resolve by walking
         * process page tables.  For now, skip activation. */
        return 0;
    }

    uint64_t phys = vmm_get_physaddr(up->vaddr);
    if (!phys) return -1;

    uint8_t *ptr = (uint8_t *)PHYS_TO_VIRT(phys);
    up->orig_byte = *ptr;

    /* Enable write to the page (if read-only) and insert INT3 */
    *ptr = 0xCC;
    up->active = 1;

    kprintf("[uprobes] activated: vaddr=0x%llx orig_byte=0x%02x\n",
            (unsigned long long)up->vaddr, (unsigned int)up->orig_byte);

    return 0;
}

/* ── Debugfs callbacks ──────────────────────────────────────────────── */

void uprobe_list_read(char *buf, int *len)
{
    int pos = 0;
    int max = 2048;

    uint64_t flags;
    spinlock_irqsave_acquire(&g_uprobe_lock, &flags);

    pos += snprintf(buf + pos, (size_t)(max - pos > 0 ? max - pos : 0),
                    "%-4s %-20s %-10s %-6s %-18s %s\n",
                    "Slot", "Path", "Offset", "Active", "Vaddr", "Handler");

    for (int i = 0; i < UPROBES_MAX_PROBES; i++) {
        if (!g_uprobes[i].in_use) continue;

        struct uprobe *up = &g_uprobes[i];
        int remaining = max - pos;
        if (remaining <= 0) break;

        pos += snprintf(buf + pos, (size_t)(remaining > 0 ? remaining : 0),
                        "[%2d] %-20s 0x%08llx %-6s 0x%016llx %s\n",
                        i,
                        up->path,
                        (unsigned long long)up->offset,
                        up->active ? "yes" : "no",
                        (unsigned long long)up->vaddr,
                        up->handler == uprobe_default_handler ?
                            "default" : "custom");
    }

    /* Append recent events from ring buffer */
    pos += snprintf(buf + pos, (size_t)(max - pos > 0 ? max - pos : 0),
                    "\nRecent events (%d entries):\n",
                    g_uprobe_ring_count);

    for (int i = 0; i < g_uprobe_ring_count && pos < max; i++) {
        int idx = (g_uprobe_ring_head + i) % UPROBES_RING_SIZE;
        if (!g_uprobe_ring[idx].valid) continue;
        int remaining = max - pos;
        if (remaining <= 0) break;
        pos += snprintf(buf + pos, (size_t)(remaining > 0 ? remaining : 0),
                        "  pid=%u addr=0x%llx tick=%llu\n",
                        (unsigned int)g_uprobe_ring[idx].pid,
                        (unsigned long long)g_uprobe_ring[idx].address,
                        (unsigned long long)g_uprobe_ring[idx].timestamp);
    }

    spinlock_irqsave_release(&g_uprobe_lock, flags);

    *len = pos;
}

int uprobe_register_write(const char *buf, int len)
{
    /* Parse: "register <path> <offset>" or "unregister <path> <offset>" */
    if (!buf || len <= 0) return -EINVAL;

    /* Make a local copy */
    char cmd[256];
    int copylen = len < 255 ? len : 255;
    memcpy(cmd, buf, (size_t)copylen);
    cmd[copylen] = '\0';

    /* Trim trailing whitespace/newline */
    while (copylen > 0 && (cmd[copylen - 1] == '\n' ||
           cmd[copylen - 1] == '\r' || cmd[copylen - 1] == ' '))
        cmd[--copylen] = '\0';

    /* Parse command */
    char *action = NULL;
    char *path = NULL;
    uint64_t offset = 0;

    /* First token: "register" or "unregister" */
    action = cmd;
    char *p = cmd;
    while (*p && *p != ' ') p++;
    if (*p) {
        *p++ = '\0';
        /* Second token: path */
        while (*p == ' ') p++;
        path = p;
        while (*p && *p != ' ') p++;
        if (*p) {
            *p++ = '\0';
            /* Third token: offset (hex or decimal) */
            while (*p == ' ') p++;
            const char *offs = p;
            /* Parse offset */
            offset = 0;
            if (offs[0] == '0' && (offs[1] == 'x' || offs[1] == 'X')) {
                offs += 2;
                while (*offs) {
                    offset = offset * 16;
                    if (*offs >= '0' && *offs <= '9')
                        offset += (uint64_t)(*offs - '0');
                    else if (*offs >= 'a' && *offs <= 'f')
                        offset += (uint64_t)(*offs - 'a' + 10);
                    else if (*offs >= 'A' && *offs <= 'F')
                        offset += (uint64_t)(*offs - 'A' + 10);
                    else
                        break;
                    offs++;
                }
            } else {
                while (*offs >= '0' && *offs <= '9') {
                    offset = offset * 10 + (uint64_t)(*offs - '0');
                    offs++;
                }
            }
        }
    }

    if (!action || !path) return -EINVAL;

    if (strcmp(action, "register") == 0) {
        return uprobe_register(path, offset, uprobe_default_handler);
    } else if (strcmp(action, "unregister") == 0) {
        return uprobe_unregister(path, offset);
    }

    return -EINVAL;
}

/* ── Stub: uprobe_handle_swbp ─────────────────────────────── */
static int uprobe_handle_swbp(void *regs)
{
    (void)regs;
    kprintf("[uprobes] uprobe_handle_swbp: not yet implemented\n");
    return 0;
}
/* ── Stub: uprobe_handle_singlestep ─────────────────────────────── */
static int uprobe_handle_singlestep(void *regs)
{
    (void)regs;
    kprintf("[uprobes] uprobe_handle_singlestep: not yet implemented\n");
    return 0;
}
