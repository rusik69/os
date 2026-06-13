/* cfi.c — Control Flow Integrity: forward-edge CFI
 *
 * Validates indirect call targets against a whitelist of valid function
 * addresses.  This provides forward-edge control-flow integrity,
 * preventing attackers from redirecting function pointers to arbitrary
 * code.
 *
 * When an indirect call is made through a registered function pointer
 * type, the target address is checked against the set of valid targets
 * for that type.
 */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "panic.h"
#include "process.h"
#include "errno.h"

/* ── Configuration ─────────────────────────────────────────────────── */

#define CFI_MAX_SLOTS          4096
#define CFI_MAX_TARGETS_PER    256
#define CFI_HASH_BUCKETS       128

/* ── CFI slot descriptor ───────────────────────────────────────────── */

struct cfi_slot {
    const char *type_name;          /* e.g. "struct file_operations" */
    uint64_t   valid_targets[CFI_MAX_TARGETS_PER];
    int        num_targets;
    uint64_t   type_hash;           /* hash of type name for fast lookup */
    int        in_use;
};

/* ── Globals ───────────────────────────────────────────────────────── */

static struct cfi_slot g_cfi_slots[CFI_MAX_SLOTS];
static int g_cfi_num_slots = 0;
static int cfi_enabled = 1;

/* Statistics */
static uint64_t cfi_checks_total = 0;
static uint64_t cfi_violations  = 0;

/* ── Hash helpers ──────────────────────────────────────────────────── */

static uint64_t cfi_hash_name(const char *name)
{
    uint64_t h = 0x9e3779b97f4a7c15ull;
    const unsigned char *p = (const unsigned char *)name;
    while (*p) {
        h ^= (uint64_t)(*p);
        h *= 0x9e3779b97f4a7c15ull;
        h ^= h >> 47;
        p++;
    }
    return h;
}

/* ── Slot management ───────────────────────────────────────────────── */

int cfi_register_type(const char *type_name,
                       const uint64_t *targets, int num_targets)
{
    if (!cfi_enabled) return -1;
    if (g_cfi_num_slots >= CFI_MAX_SLOTS) return -1;
    if (num_targets > CFI_MAX_TARGETS_PER) return -1;
    if (!type_name || !targets) return -EINVAL;

    int idx = g_cfi_num_slots;
    struct cfi_slot *slot = &g_cfi_slots[idx];

    slot->type_name = type_name;
    slot->type_hash = cfi_hash_name(type_name);
    slot->num_targets = num_targets;
    slot->in_use = 1;

    for (int i = 0; i < num_targets; i++) {
        slot->valid_targets[i] = targets[i];
    }

    /* Sort targets for binary search */
    for (int i = 0; i < num_targets - 1; i++) {
        for (int j = 0; j < num_targets - 1 - i; j++) {
            if (slot->valid_targets[j] > slot->valid_targets[j + 1]) {
                uint64_t tmp = slot->valid_targets[j];
                slot->valid_targets[j] = slot->valid_targets[j + 1];
                slot->valid_targets[j + 1] = tmp;
            }
        }
    }

    g_cfi_num_slots++;
    return idx;
}

/* ── Target validation ─────────────────────────────────────────────── */

int cfi_check_target(const char *type_name, uint64_t target_addr)
{
    cfi_checks_total++;

    if (!cfi_enabled) return 1;  /* allow if disabled */
    if (!type_name) return 1;    /* allow untyped indirect calls */

    uint64_t type_hash = cfi_hash_name(type_name);

    for (int i = 0; i < g_cfi_num_slots; i++) {
        struct cfi_slot *slot = &g_cfi_slots[i];
        if (!slot->in_use) continue;
        if (slot->type_hash != type_hash) continue;
        if (strcmp(slot->type_name, type_name) != 0) continue;

        /* Binary search in sorted targets */
        int lo = 0, hi = slot->num_targets - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (slot->valid_targets[mid] == target_addr) {
                return 1;  /* valid target */
            } else if (slot->valid_targets[mid] < target_addr) {
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }

        /* Target not in whitelist — violation */
        cfi_violations++;
        return 0;
    }

    /* Type not registered — allow (no CFI policy for this type) */
    return 1;
}

/* ── Violation handler ─────────────────────────────────────────────── */

void cfi_handle_violation(const char *type_name, uint64_t target_addr,
                           uint64_t caller_addr)
{
    struct process *p = process_get_current();

    kprintf("[CFI] VIOLATION: type='%s' target=0x%016llx caller=0x%016llx pid=%d\n",
            type_name ? type_name : "?",
            (unsigned long long)target_addr,
            (unsigned long long)caller_addr,
            p ? p->pid : -1);

    /* In production, kill the process */
    if (p) {
        process_kill(p->pid, 6); /* SIGABRT */
    }
}

/* ── Query / status ────────────────────────────────────────────────── */

int cfi_get_enabled(void) { return cfi_enabled; }
void cfi_set_enabled(int val) { cfi_enabled = val ? 1 : 0; }
uint64_t cfi_get_checks(void) { return cfi_checks_total; }
uint64_t cfi_get_violations(void) { return cfi_violations; }
int cfi_get_num_slots(void) { return g_cfi_num_slots; }

/* ── Initialization ────────────────────────────────────────────────── */

void cfi_init(void)
{
    memset(g_cfi_slots, 0, sizeof(g_cfi_slots));
    g_cfi_num_slots = 0;
    cfi_checks_total = 0;
    cfi_violations = 0;

    kprintf("[OK] CFI: forward-edge control flow integrity initialized (%d slots max)\n",
            CFI_MAX_SLOTS);
}
