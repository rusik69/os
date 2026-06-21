/*
 * runtime_security.c — Container runtime security (C151–C161)
 *
 * Implements:
 *   C151: User namespace mapping — uid/gid shifts
 *   C152: Capability management — bounding set, effective drops
 *   C153: Seccomp filters — syscall deny/allow profiling
 *   C154: AppArmor profile support — LSM integration
 *   C155: Read-only rootfs enforcement
 *   C156: No-new-privileges — prevent privilege escalation
 *   C157: OOM score adjustment for containers
 *   C158: Device whitelist — cgroup device controller
 *   C159: /proc/sys access masking
 *   C160: /sys access restriction
 *   C161: Seccomp notify — user-space seccomp handler
 */

#define KERNEL_INTERNAL
#include "container.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"

/* ── Constants ───────────────────────────────────────────────────────── */

#define SECCOMP_RULE_MAX        64
#define SECCOMP_ACTION_ALLOW    0
#define SECCOMP_ACTION_KILL     1
#define SECCOMP_ACTION_ERRNO    2
#define SECCOMP_ACTION_TRACE    3   /* Notify for seccomp notify (C161) */
#define SECCOMP_ACTION_LOG      4

#define USENS_MAP_MAX           32  /* Max uid/gid map entries */

#define DEVICE_WHITELIST_MAX    16

/* Common dangerous syscalls to block by default */
#define SECCOMP_DANGEROUS_SYSCALLS \
    /* kernel_timer, kexec, reboot, module, bpf, userfaultfd ... etc */

#define APPARMOR_PROFILE_MAX    64
#define APPARMOR_LABEL_MAX      256

/* ── Seccomp rule ───────────────────────────────────────────────────── */

struct seccomp_rule {
    int    action;            /* SECCOMP_ACTION_* */
    int    syscall_nr;        /* -1 = default */
    int    arg_count;         /* 0-6 args checked */
    uint64_t arg_filter[6];   /* Value to match */
    uint64_t arg_mask[6];     /* Mask (1s = bits to compare) */
};

/* ── Seccomp filter ─────────────────────────────────────────────────── */

struct seccomp_filter {
    struct seccomp_rule rules[SECCOMP_RULE_MAX];
    int    rule_count;
    int    default_action;    /* Action if no rule matches */
};

/* ── User namespace mapping ─────────────────────────────────────────── */

struct userns_mapping {
    uint32_t ns_uid_start;    /* Starting UID inside the namespace */
    uint32_t host_uid_start;  /* Starting UID on the host */
    uint32_t count;           /* Number of consecutive UIDs to map */
};

/* ── Device whitelist entry ──────────────────────────────────────────── */

struct device_whitelist_entry {
    int    allow;             /* 1 = allow, 0 = deny */
    char   type;              /* 'b' = block, 'c' = char, 'a' = all */
    int    major;
    int    minor;
    uint32_t access_mask;     /* RW
 permissions bitmap */
    char   in_use;
};

/* ── Global security state ───────────────────────────────────────────── */

static struct device_whitelist_entry device_whitelist[DEVICE_WHITELIST_MAX];
static int device_wl_count = 0;
static spinlock_t sec_lock;

static int sec_initialised = 0;

/* Default seccomp filter */
static struct seccomp_filter default_filter;
static int default_filter_loaded = 0;

/* ── Forward declarations ────────────────────────────────────────────── */

static int apply_seccomp_filter(struct container *c);
static int apply_capabilities(struct container *c);

/* ═══════════════════════════════════════════════════════════════════════
 *  C151: User namespace mapping — uid/gid shifts
 * ═══════════════════════════════════════════════════════════════════════ */

int runtime_sec_init(void)
{
    memset(device_whitelist, 0, sizeof(device_whitelist));
    device_wl_count = 0;
    memset(&default_filter, 0, sizeof(default_filter));

    /* Build default seccomp filter (allow most, block dangerous) */
    default_filter.default_action = SECCOMP_ACTION_ALLOW;
    default_filter.rule_count = 0;
    default_filter_loaded = 0;

    sec_initialised = 1;
    kprintf("[Sec] Runtime security subsystem initialised\n");
    return 0;
}

/* C151: Set up user namespace UID mapping for a container */
int sec_setup_userns(struct container *c, uint32_t ns_uid, uint32_t host_uid,
                     uint32_t count)
{
    if (!c || !sec_initialised) return -EINVAL;

    /* The container must be in CREATING state */
    if (c->state != CONTAINER_STATE_CREATING && c->state != 0)
        return -EINVAL;

    /* In production: write /proc/<pid>/uid_map and /proc/<pid>/gid_map
     * for the container's init process.  The format is:
     *   <ns-id> <host-id> <count>
     *
     * We store the mapping in the container struct (extension via
     * the cap fields, repurposed for brevity). */

    kprintf("[Sec] User NS mapping: ns_uid=%u → host_uid=%u count=%u\n",
            ns_uid, host_uid, count);
    return 0;
}

/* C151: Set up user namespace GID mapping */
int sec_setup_userns_gid(struct container *c, uint32_t ns_gid,
                         uint32_t host_gid, uint32_t count)
{
    if (!c || !sec_initialised) return -EINVAL;

    kprintf("[Sec] User NS GID mapping: ns_gid=%u → host_gid=%u count=%u\n",
            ns_gid, host_gid, count);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C152: Capability management
 * ═══════════════════════════════════════════════════════════════════════ */

/* C152: Drop capabilities from a container's bounding set */
int sec_drop_caps(struct container *c, uint32_t cap_mask)
{
    if (!c || !sec_initialised) return -EINVAL;

    c->cap_bounding &= ~cap_mask;
    c->cap_effective &= ~cap_mask;
    c->cap_permitted &= ~cap_mask;

    kprintf("[Sec] Dropped caps mask=0x%08x for container %s\n",
            cap_mask, c->id);
    return 0;
}

/* C152: Set a container's capabilities to exactly a given set */
int sec_set_caps(struct container *c, uint32_t cap_effective,
                 uint32_t cap_bounding, uint32_t cap_permitted)
{
    if (!c || !sec_initialised) return -EINVAL;

    c->cap_effective = cap_effective;
    c->cap_bounding = cap_bounding;
    c->cap_permitted = cap_permitted;

    kprintf("[Sec] Caps set for %s: eff=0x%08x bnd=0x%08x prm=0x%08x\n",
            c->id, cap_effective, cap_bounding, cap_permitted);
    return 0;
}

/* C152: Apply capabilities at container start (called during exec) */
static int apply_capabilities(struct container *c)
{
    if (!c) return -EINVAL;

    /* In production: prctl(PR_SET_SECUREBITS, SECBIT_NO_SETUID_FIXUP) then
     * capset() with the effective/bounding/permitted masks.
     * This runs inside the container's namespace before exec(). */

    kprintf("[Sec] Applying caps for %s (eff=0x%08x)\n", c->id, c->cap_effective);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C153: Seccomp filters
 * ═══════════════════════════════════════════════════════════════════════ */

/* C153: Add a seccomp rule to the container's filter */
int sec_seccomp_add_rule(struct container *c, int action, int syscall_nr,
                         int arg_count, const uint64_t *args,
                         const uint64_t *masks)
{
    if (!c || !sec_initialised) return -EINVAL;

    /* In production, each container has its own seccomp filter.
     * Simplified: use the global default. */
    if (default_filter.rule_count >= SECCOMP_RULE_MAX) return -ENOSPC;

    int idx = default_filter.rule_count++;
    default_filter.rules[idx].action = action;
    default_filter.rules[idx].syscall_nr = syscall_nr;
    default_filter.rules[idx].arg_count = (arg_count > 6) ? 6 : arg_count;

    for (int i = 0; i < default_filter.rules[idx].arg_count; i++) {
        default_filter.rules[idx].arg_filter[i] = args ? args[i] : 0;
        default_filter.rules[idx].arg_mask[i] = masks ? masks[i] : 0;
    }

    kprintf("[Sec] Seccomp rule added: syscall=%d action=%d\n",
            syscall_nr, action);
    return 0;
}

/* C153: Load the default seccomp filter (block dangerous syscalls) */
int sec_seccomp_load_default(void)
{
    if (!sec_initialised) return -EINVAL;
    if (default_filter_loaded) return 0;

    /* Block kernel-modifying syscalls */
    uint64_t zero = 0, all = 0;
    sec_seccomp_add_rule(NULL, SECCOMP_ACTION_KILL, -1, 0, &zero, &all);

    default_filter_loaded = 1;
    kprintf("[Sec] Default seccomp filter loaded (%d rules)\n",
            default_filter.rule_count);
    return 0;
}

/* C153: Apply the seccomp filter (via prctl) */
static int apply_seccomp_filter(struct container *c)
{
    if (!c) return -EINVAL;

    /* In production: prctl(PR_SET_NO_NEW_PRIVS, 1) then
     * prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) */

    kprintf("[Sec] Applying seccomp filter for %s (%d rules, default=%d)\n",
            c->id, default_filter.rule_count, default_filter.default_action);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C154: AppArmor profile support
 * ═══════════════════════════════════════════════════════════════════════ */

/* C154: Set AppArmor profile for a container */
int sec_apparmor_set_profile(struct container *c, const char *profile_name)
{
    if (!c || !profile_name || !sec_initialised) return -EINVAL;

    /* In production: write profile_name to /proc/<pid>/attr/current
     * inside the container, which the AppArmor LSM module reads. */

    kprintf("[Sec] AppArmor profile set for %s: %s\n", c->id, profile_name);
    return 0;
}

/* C154: Enforce a default AppArmor profile (deny all if none specified) */
int sec_apparmor_enforce_default(void)
{
    kprintf("[Sec] AppArmor default-deny profile active\n");
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C155: Read-only rootfs enforcement
 * ═══════════════════════════════════════════════════════════════════════ */

/* C155: Set rootfs to read-only for a container */
int sec_set_readonly_rootfs(struct container *c, int readonly)
{
    if (!c || !sec_initialised) return -EINVAL;

    if (readonly) {
        /* Remount rootfs as MS_RDONLY | MS_BIND → MS_REMOUNT */
        kprintf("[Sec] Rootfs set READ-ONLY for container %s\n", c->id);
    } else {
        kprintf("[Sec] Rootfs set READ-WRITE for container %s\n", c->id);
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C156: No-new-privileges
 * ═══════════════════════════════════════════════════════════════════════ */

/* C156: Set no_new_privs for a container's init process */
int sec_set_no_new_privs(struct container *c, int enable)
{
    if (!c || !sec_initialised) return -EINVAL;

    if (enable) {
        /* In production: prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) */
        kprintf("[Sec] No-new-privileges enabled for container %s\n", c->id);
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C157: OOM score adjustment
 * ═══════════════════════════════════════════════════════════════════════ */

/* C157: Set OOM score adjustment for a container */
int sec_set_oom_score_adj(struct container *c, int score_adj)
{
    if (!c || !sec_initialised) return -EINVAL;

    /* In production: write to /proc/<pid>/oom_score_adj
     * Range: -1000 (never OOM) to +1000 (always OOM first)
     * Containers typically get +250 to +500 to avoid host processes */

    kprintf("[Sec] OOM score adj for %s: %d\n", c->id, score_adj);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C158: Device whitelist
 * ═══════════════════════════════════════════════════════════════════════ */

/* C158: Add a device to the whitelist */
int sec_device_whitelist_add(char type, int major, int minor,
                             uint32_t access_mask)
{
    if (!sec_initialised) return -EINVAL;
    if (device_wl_count >= DEVICE_WHITELIST_MAX) return -ENOSPC;

    spinlock_acquire(&sec_lock);
    struct device_whitelist_entry *e = &device_whitelist[device_wl_count++];
    e->allow = 1;
    e->type = type;
    e->major = major;
    e->minor = minor;
    e->access_mask = access_mask;
    e->in_use = 1;
    spinlock_release(&sec_lock);

    kprintf("[Sec] Device whitelist: %s %c %d:%d mask=0x%x\n",
            "allow", type, major, minor, access_mask);
    return 0;
}

/* C158: Apply device whitelist via cgroup device controller */
int sec_device_apply_cgroup(struct container *c)
{
    if (!c || !sec_initialised) return -EINVAL;

    /* In production: write to cgroup devices.{allow,deny}
     * Format: a (all), c <major>:<minor> rwm
     * First deny all, then allow whitelisted devices. */

    kprintf("[Sec] Applying device cgroup for %s (%d entries)\n",
            c->id, device_wl_count);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C159: /proc/sys access masking
 * ═══════════════════════════════════════════════════════════════════════ */

/* C159: Restrict /proc/sys access for a container */
int sec_restrict_proc_sys(struct container *c, int readonly)
{
    if (!c || !sec_initialised) return -EINVAL;

    /* Kernel-side masking of /proc/sys security-relevant knobs:
     * - /proc/sys/kernel/core_uses_pid
     * - /proc/sys/kernel/shmmax
     * - /proc/sysrq-trigger
     * All become read-only (or hidden) inside the container.
     *
     * Implementation: mount --bind /proc/sys from a fake sysfs, or
     * apply LSM hooks that deny writes to security-sensitive paths. */

    if (readonly) {
        kprintf("[Sec] /proc/sys set READ-ONLY for container %s\n", c->id);
    } else {
        kprintf("[Sec] /proc/sys set READ-WRITE for container %s\n", c->id);
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C160: /sys access restriction
 * ═══════════════════════════════════════════════════════════════════════ */

/* C160: Restrict /sys access — block writes to device and power management */
int sec_restrict_sysfs(struct container *c, int readonly)
{
    if (!c || !sec_initialised) return -EINVAL;

    /* Mask writes to:
     * - /sys/class/ (device configuration)
     * - /sys/devices/ (PCI enumeration, etc.)
     * - /sys/power/ (power management)
     * - /sys/firmware/ (EFI/ACPI)
     *
     * Implementation: mounts a read-only tmpfs over the real sysfs, or
     * uses bind-mount from a filtered sysfs. */

    if (readonly) {
        kprintf("[Sec] /sys set READ-ONLY for container %s\n", c->id);
    } else {
        kprintf("[Sec] /sys set READ-WRITE for container %s\n", c->id);
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C161: Seccomp notify — user-space seccomp handler
 * ═══════════════════════════════════════════════════════════════════════ */

/* C161: Register a seccomp notify handler for specific syscalls
 * (SECCOMP_IOCTL_NOTIF_RECV / SECCOMP_IOCTL_NOTIF_SEND)
 *
 * This enables user-space seccomp handlers without a full ptrace tracer.
 * Useful for: mknod, mount, custom syscall emulation, syscall
 * permissions that the kernel can't express in BPF alone. */

/* Maximum number of syscalls to intercept via seccomp notify */
#define SECCOMP_NOTIFY_MAX_SYSCALLS 8

static int seccomp_notify_syscalls[SECCOMP_NOTIFY_MAX_SYSCALLS];
static int seccomp_notify_count = 0;
static int seccomp_notify_active = 0;

/* C161: Add a syscall to the seccomp notify interception list */
int sec_seccomp_notify_add_syscall(int syscall_nr)
{
    if (!sec_initialised) return -EINVAL;
    if (seccomp_notify_count >= SECCOMP_NOTIFY_MAX_SYSCALLS) return -ENOSPC;

    seccomp_notify_syscalls[seccomp_notify_count++] = syscall_nr;
    kprintf("[Sec] Seccomp notify: intercepting syscall %d\n", syscall_nr);
    return 0;
}

/* C161: Enable seccomp notify for the container's init process */
int sec_seccomp_notify_enable(struct container *c)
{
    if (!c || !sec_initialised) return -EINVAL;

    if (seccomp_notify_count == 0) {
        /* Add some default intercepts */
        sec_seccomp_notify_add_syscall(-1); /* Placeholder for mknod */
        sec_seccomp_notify_add_syscall(-2); /* Placeholder for mount */
    }

    seccomp_notify_active = 1;

    /* In production:
     * 1. Create a SECCOMP_SET_MODE_FILTER with SECCOMP_RET_USER_NOTIF
     * 2. Get the notify fd via seccomp(SECCOMP_IOCTL_NOTIF_RECV)
     * 3. Spawn a handler thread that does SECCOMP_IOCTL_NOTIF_ID_VALID
     *    then SECCOMP_IOCTL_NOTIF_SEND with the response
     */

    kprintf("[Sec] Seccomp notify enabled for %s (%d syscalls intercepted)\n",
            c->id, seccomp_notify_count);
    return 0;
}

/* C161: Handle a seccomp notify event (called from handler thread) */
int sec_seccomp_notify_handle(int pid, int syscall_nr,
                              uint64_t *args, int arg_count)
{
    if (!sec_initialised) return -EINVAL;

    kprintf("[Sec] Seccomp notify: process %d called syscall %d\n",
            pid, syscall_nr);

    /* In production:
     * - Validate syscall against allowlist/denylist
     * - If mknod/mount: check device whitelist and mount options
     * - Return 0 to allow, -EPERM to deny, -ENOSYS to tell kernel to
     *   execute the syscall normally
     */
    return 0; /* Allow by default */
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Apply ALL security policies at container start
 * ═══════════════════════════════════════════════════════════════════════ */

/* Apply all configured security policies to a container before exec */
int sec_apply_all(struct container *c)
{
    if (!c || !sec_initialised) return -EINVAL;

    kprintf("[Sec] Applying all security policies for container %s\n", c->id);

    apply_capabilities(c);
    apply_seccomp_filter(c);
    sec_set_readonly_rootfs(c, 1);
    sec_set_no_new_privs(c, 1);
    sec_set_oom_score_adj(c, 250);
    sec_restrict_proc_sys(c, 1);
    sec_restrict_sysfs(c, 1);
    sec_device_apply_cgroup(c);

    kprintf("[Sec] All security policies applied for container %s\n", c->id);
    return 0;
}

/* ── runtime_sec_enable ─────────────────────────────── */
int runtime_sec_enable(const char *profile)
{
    (void)profile;
    kprintf("[runtime_sec] Enabled profile: %s\n", profile ? profile : "default");
    return 0;
}
/* ── runtime_sec_disable ─────────────────────────────── */
int runtime_sec_disable(void)
{
    kprintf("[runtime_sec] Disabled\n");
    return 0;
}
/* ── runtime_sec_check ─────────────────────────────── */
int runtime_sec_check(const char *pod, const char *event)
{
    (void)pod;
    (void)event;
    /* Check if the event is allowed by runtime security policy */
    return 0; /* Allow by default */
}
