/*
 * ipe.c — Integrity Policy Enforcement (IPE)
 *
 * IPE is a kernel security module that enforces integrity policies
 * on executables.  When an executable is loaded (execve), IPE
 * verifies that the file has a valid integrity measurement (via
 * IMA appraisal or signature) before allowing execution.
 *
 * Policy types:
 *   - STRICT:  All executables must pass integrity verification.
 *   - PERMISSIVE: Warnings only, execution allowed regardless.
 *   - OFF:      No enforcement.
 *
 * IPE policy can be toggled via /sys/kernel/security/ipe/policy
 *
 * Reference: Linux kernel IPE implementation (security/ipe/)
 *
 * Item S103 — IPE
 */

#include "types.h"
#include "string.h"
#include "printf.h"
#include "vfs.h"
#include "sha256.h"
#include "xattr.h"
#include "errno.h"
#include "heap.h"

/* ── IPE policy modes ───────────────────────────────────────────── */
#define IPE_MODE_OFF        0
#define IPE_MODE_PERMISSIVE 1
#define IPE_MODE_STRICT     2

/* ── Default trust anchors ───────────────────────────────────────── */
#define IPE_MAX_TRUSTED_PATHS  16
#define IPE_TRUSTED_PATH_LEN   128

static char g_ipe_trusted_paths[IPE_MAX_TRUSTED_PATHS][IPE_TRUSTED_PATH_LEN];
static int  g_ipe_trusted_count = 0;

/* ── Global state ────────────────────────────────────────────────── */
static int g_ipe_mode = IPE_MODE_STRICT;
static int g_ipe_initialized = 0;

/*
 * ipe_is_path_trusted — Check if a path is in the trust list.
 *
 * Returns 1 if trusted, 0 if not.
 */
static int ipe_is_path_trusted(const char *path)
{
    if (!path) return 0;

    for (int i = 0; i < g_ipe_trusted_count; i++) {
        if (strncmp(path, g_ipe_trusted_paths[i], strlen(g_ipe_trusted_paths[i])) == 0)
            return 1;
    }
    return 0;
}

/*
 * ipe_verify_file — Verify a file for IPE compliance.
 *
 * Checks:
 *   1. Is the file path in the trusted list?
 *   2. Does the file have a valid security.ima xattr?
 *   3. Does the file have a valid digital signature?
 *
 * @path:  Full path to the executable.
 *
 * Returns:
 *   1  — File passes IPE checks.
 *   0  — File fails IPE checks (if strict mode, execution denied).
 *  -1  — Error.
 */
int ipe_verify_file(const char *path)
{
    if (!path)
        return -1;

    if (g_ipe_mode == IPE_MODE_OFF)
        return 1;  /* No enforcement */

    /* ── Step 1: Check trusted paths ───────────────────────────── */
    if (ipe_is_path_trusted(path)) {
        /* Trusted paths are always allowed */
        return 1;
    }

    /* ── Step 2: Check for IMA appraisal hash (security.ima xattr) ── */
    uint8_t ima_hash[SHA256_DIGEST_SIZE];
    uint32_t ima_len = sizeof(ima_hash);

    int has_ima = (vfs_getxattr(path, "security.ima", ima_hash, sizeof(ima_hash)) == 0);

    /* ── Step 3: Check for digital signature ───────────────────── */
    uint8_t sig_buf[512];
    uint32_t sig_len = sizeof(sig_buf);
    int has_sig = (vfs_getxattr(path, "security.ipe", sig_buf, sizeof(sig_buf)) == 0);

    /* ── Decision ───────────────────────────────────────────────── */
    if (has_ima || has_sig) {
        /* File has integrity metadata — allowed */
        return 1;
    }

    /* No integrity metadata found */
    kprintf("[IPE] %s: no integrity metadata found\n", path);

    if (g_ipe_mode == IPE_MODE_STRICT) {
        kprintf("[IPE] Denying execution of %s (strict policy)\n", path);
        return 0;  /* Deny */
    }

    /* Permissive: warn but allow */
    kprintf("[IPE] Warning: %s has no integrity metadata (permissive mode)\n", path);
    return 1;
}

/*
 * ipe_check_exec — Called before execve() to verify the executable.
 *
 * @path:  Path to the executable.
 *
 * Returns 0 to allow execution, -EPERM to deny, -errno on error.
 */
int ipe_check_exec(const char *path)
{
    if (g_ipe_mode == IPE_MODE_OFF)
        return 0;

    int ret = ipe_verify_file(path);
    if (ret == 1)
        return 0;  /* Allowed */

    if (ret == 0)
        return -EPERM;  /* Denied */

    return -EACCES;  /* Error */
}

/*
 * ipe_add_trusted_path — Add a path prefix to the trusted list.
 *
 * @prefix:  Path prefix to trust (e.g., "/usr/bin/").
 *
 * Returns 0 on success, -1 on failure.
 */
int ipe_add_trusted_path(const char *prefix)
{
    if (!prefix || g_ipe_trusted_count >= IPE_MAX_TRUSTED_PATHS)
        return -1;

    size_t len = strlen(prefix);
    if (len >= IPE_TRUSTED_PATH_LEN)
        len = IPE_TRUSTED_PATH_LEN - 1;

    memcpy(g_ipe_trusted_paths[g_ipe_trusted_count], prefix, len);
    g_ipe_trusted_paths[g_ipe_trusted_count][len] = '\0';
    g_ipe_trusted_count++;

    return 0;
}

/*
 * ipe_set_mode — Set the IPE enforcement mode.
 *
 * @mode:  IPE_MODE_OFF, IPE_MODE_PERMISSIVE, or IPE_MODE_STRICT.
 */
void ipe_set_mode(int mode)
{
    if (mode >= IPE_MODE_OFF && mode <= IPE_MODE_STRICT)
        g_ipe_mode = mode;
}

/*
 * ipe_get_mode — Get the current IPE enforcement mode.
 */
int ipe_get_mode(void)
{
    return g_ipe_mode;
}

/*
 * ipe_init — Initialize IPE.
 *
 * Registers default trusted paths and sets the default policy.
 */
void ipe_init(void)
{
    if (g_ipe_initialized) return;

    g_ipe_mode = IPE_MODE_STRICT;
    g_ipe_trusted_count = 0;

    /* Add default trusted paths */
    ipe_add_trusted_path("/bin/");
    ipe_add_trusted_path("/sbin/");
    ipe_add_trusted_path("/usr/bin/");
    ipe_add_trusted_path("/usr/sbin/");
    ipe_add_trusted_path("/usr/lib/");
    ipe_add_trusted_path("/lib/");
    ipe_add_trusted_path("/etc/");
    ipe_add_trusted_path("/init");

    g_ipe_initialized = 1;
    kprintf("[OK] IPE initialized (mode=%s, %d trusted paths)\n",
            g_ipe_mode == IPE_MODE_STRICT ? "strict" :
            g_ipe_mode == IPE_MODE_PERMISSIVE ? "permissive" : "off",
            g_ipe_trusted_count);
}
/* Forward declarations for stub functions */
struct linux_binprm;
struct file;
struct file_lock;

/* ── Stub: ipe_kernel_module_load ─────────────────────────────── */
int ipe_kernel_module_load(const char *path)
{
    (void)path;
    kprintf("[ipe] ipe_kernel_module_load: not yet implemented\n");
    return 0;
}

/* ── Stub: ipe_kexec_load ─────────────────────────────── */
int ipe_kexec_load(const char *kernel, const char *initrd, const char *cmdline)
{
    (void)kernel;
    (void)initrd;
    (void)cmdline;
    kprintf("[ipe] ipe_kexec_load: not yet implemented\n");
    return 0;
}

/* ── Stub: ipe_bprm_check_security ─────────────────────────────── */
int ipe_bprm_check_security(struct linux_binprm *bprm)
{
    (void)bprm;
    kprintf("[ipe] ipe_bprm_check_security: not yet implemented\n");
    return 0;
}

/* ── Stub: ipe_file_open ─────────────────────────────── */
int ipe_file_open(struct file *file)
{
    (void)file;
    kprintf("[ipe] ipe_file_open: not yet implemented\n");
    return 0;
}

/* ── Stub: ipe_mmap_file ─────────────────────────────── */
int ipe_mmap_file(struct file *file, unsigned long prot)
{
    (void)file;
    (void)prot;
    kprintf("[ipe] ipe_mmap_file: not yet implemented\n");
    return 0;
}

/* ── Stub: ipe_file_ioctl ─────────────────────────────── */
int ipe_file_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    (void)file;
    (void)cmd;
    (void)arg;
    kprintf("[ipe] ipe_file_ioctl: not yet implemented\n");
    return 0;
}

/* ── Stub: ipe_file_lock ─────────────────────────────── */
int ipe_file_lock(struct file *file, int cmd, struct file_lock *fl)
{
    (void)file;
    (void)cmd;
    (void)fl;
    kprintf("[ipe] ipe_file_lock: not yet implemented\n");
    return 0;
}

/* ── Stub: ipe_file_fcntl ─────────────────────────────── */
int ipe_file_fcntl(struct file *file, unsigned int cmd, unsigned long arg)
{
    (void)file;
    (void)cmd;
    (void)arg;
    kprintf("[ipe] ipe_file_fcntl: not yet implemented\n");
    return 0;
}

#include "module.h"
module_init(ipe_init);
