/*
 * src/drivers/vmw_balloon.c — VMware balloon driver
 *
 * Implements the VMware balloon memory management driver.
 * Uses MMIO communication with the hypervisor (backdoor I/O)
 * to dynamically adjust guest memory pressure.  Reports
 * statistics (balloon size, target, free memory).
 * Follows existing vmw/hypervisor driver patterns.
 */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "io.h"
#include "pmm.h"
#include "vmm.h"

#ifdef MODULE
#include "module.h"
#endif

/* ── VMware backdoor I/O port ──────────────────────────────────── */

#define VMW_PORT           0x5658   /* VMware backdoor port */

/* VMware hypervisor call magic */
#define VMW_MAGIC          0x564D5868   /* "VMXh" */

/* Backdoor commands (BDOOR_CMD) */
#define VMW_CMD_GET_MEMSIZE        1
#define VMW_CMD_GET_BALLOON_TARGET 2
#define VMW_CMD_BALLOON_LOCK       3
#define VMW_CMD_BALLOON_UNLOCK     4
#define VMW_CMD_GET_GUEST_INFO     5

/* ── Balloon state constants ───────────────────────────────────── */

#define BALLOON_PAGES_PER_CHUNK    256

/* ── Driver state ──────────────────────────────────────────────── */

static int      balloon_detected     = 0;
static uint32_t balloon_current      = 0;
static uint32_t balloon_target       = 0;

/* Stats counters */
static uint32_t stat_inflate  = 0;
static uint32_t stat_deflate  = 0;

/* ── Backdoor I/O communication ────────────────────────────────── */

static uint32_t vmw_backdoor(uint32_t cmd, uint32_t arg1,
                              uint32_t arg2, uint32_t *out)
{
    uint32_t result;
    uint32_t dummy;

    __asm__ volatile("inl %%dx, %%eax"
                 : "=a"(result), "=b"(dummy)
                 : "a"(VMW_MAGIC), "b"(arg1), "c"(cmd), "d"(VMW_PORT)
                 : "memory");

    if (out) *out = dummy;
    return result;
}

/* ── VMware detection ──────────────────────────────────────────── */

static int vmw_detect(void)
{
    uint32_t ret = vmw_backdoor(VMW_CMD_GET_GUEST_INFO, 0, 0, NULL);
    return (ret != 0);
}

/* ── Balloon operations ────────────────────────────────────────── */

static uint32_t vmw_balloon_get_target(void)
{
    uint32_t target;
    vmw_backdoor(VMW_CMD_GET_BALLOON_TARGET, 0, 0, &target);
    return target;
}

static void vmw_balloon_inflate(uint32_t pages)
{
    for (uint32_t i = 0; i < pages; i += BALLOON_PAGES_PER_CHUNK) {
        uint32_t chunk = (pages - i > BALLOON_PAGES_PER_CHUNK)
                         ? BALLOON_PAGES_PER_CHUNK : (pages - i);
        vmw_backdoor(VMW_CMD_BALLOON_LOCK, 0, chunk, NULL);
        balloon_current += chunk;
        stat_inflate += chunk;
    }
}

static void vmw_balloon_deflate(uint32_t pages)
{
    for (uint32_t i = 0; i < pages; i += BALLOON_PAGES_PER_CHUNK) {
        uint32_t chunk = (pages - i > BALLOON_PAGES_PER_CHUNK)
                         ? BALLOON_PAGES_PER_CHUNK : (pages - i);
        vmw_backdoor(VMW_CMD_BALLOON_UNLOCK, 0, chunk, NULL);
        if (balloon_current >= chunk)
            balloon_current -= chunk;
        stat_deflate += chunk;
    }
}

/* ── Stats reporting ───────────────────────────────────────────── */

void vmw_balloon_stats(void)
{
    kprintf("[vmw-balloon] stats: current=%u target=%u "
            "inflate=%u deflate=%u\n",
            balloon_current, balloon_target,
            stat_inflate, stat_deflate);
}

/* ── Init ──────────────────────────────────────────────────────── */

void vmw_balloon_init(void)
{
    if (!vmw_detect()) {
        kprintf("[vmw-balloon] not detected\n");
        return;
    }

    balloon_detected = 1;
    balloon_target = vmw_balloon_get_target();

    kprintf("[vmw-balloon] VMware balloon driver initialized: "
            "target=%u pages\n", balloon_target);
}

#ifdef MODULE
int init_module(void) { vmw_balloon_init(); return 0; }
void cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("VMware balloon — MMIO, stats");
MODULE_VERSION("1.0");
#endif
