/*
 * evm.c — Extended Verification Module (EVM)
 *
 * EVM protects extended attributes (xattrs) from offline tampering
 * by providing an HMAC-signed integrity verification.  An HMAC-SHA256
 * is computed over a set of security-relevant xattrs (security.selinux,
 * security.ima, security.capability, etc.) and stored in the
 * security.evm extended attribute.  On access, the HMAC is verified
 * to detect offline modifications.
 *
 * Reference: Linux kernel EVM implementation (security/integrity/evm/)
 *
 * Item S102 — EVM
 */

#include "evm.h"

#include "errno.h"
#include "heap.h"
#include "hmac.h"
#include "printf.h"
#include "sha256.h"
#include "string.h"
#include "types.h"
#include "vfs.h"
#include "xattr.h"

/* ── EVM key (16 bytes, stored in kernel keyring) ────────────────── */
#define EVM_KEY_SIZE  16

static uint8_t g_evm_key[EVM_KEY_SIZE];
static int     g_evm_key_set = 0;
static int     g_evm_initialized = 0;
static int     g_evm_enforce = 1;   /* 1 = deny on HMAC mismatch */

/* ── Xattrs covered by EVM ───────────────────────────────────────── */
#define EVM_XATTR_MAX  4
static const char *g_evm_protected_xattrs[EVM_XATTR_MAX] = {
    "security.selinux",
    "security.IMA",
    "security.capability",
    "security.apparmor",
};

/*
 * evm_compute_hmac — Compute HMAC-SHA256 over the protected xattrs
 * of a file.
 *
 * @path:       File path.
 * @hmac_out:   Output buffer (SHA256_DIGEST_SIZE bytes).
 *
 * Returns 0 on success, -1 on error.
 */
static int evm_compute_hmac(const char *path, uint8_t hmac_out[SHA256_DIGEST_SIZE])
{
    if (!path || !g_evm_key_set)
        return -EINVAL;

    /* Collect all protected xattr values into a buffer */
    uint8_t *buf = NULL;
    uint32_t buf_len = 0;
    uint32_t buf_cap = 256;

    buf = (uint8_t *)kmalloc(buf_cap);
    if (!buf) return -ENOMEM;

    for (int i = 0; i < EVM_XATTR_MAX; i++) {
        uint8_t xattr_val[256];

        int xret = vfs_getxattr(path, g_evm_protected_xattrs[i], xattr_val, sizeof(xattr_val));
        if (xret < 0)
            continue;  /* xattr doesn't exist — skip */
        uint32_t xattr_len = (uint32_t)xret;

        /* Append name + value to buffer */
        uint32_t name_len = (uint32_t)strlen(g_evm_protected_xattrs[i]);
        uint32_t needed = name_len + 1 + xattr_len;

        if (buf_len + needed > buf_cap) {
            buf_cap = buf_len + needed + 128;
            uint8_t *new_buf = (uint8_t *)krealloc(buf, buf_cap);
            if (!new_buf) { kfree(buf); return -ENOMEM; }
            buf = new_buf;
        }

        memcpy(buf + buf_len, g_evm_protected_xattrs[i], name_len);
        buf_len += name_len;
        buf[buf_len++] = 0;  /* NUL separator */
        memcpy(buf + buf_len, xattr_val, xattr_len);
        buf_len += xattr_len;
    }

    if (buf_len == 0) {
        /* No protected xattrs — use empty string */
        hmac_sha256(g_evm_key, EVM_KEY_SIZE,
                    (const uint8_t *)"", 0, hmac_out);
        kfree(buf);
        return 0;
    }

    /* Compute HMAC-SHA256 */
    hmac_sha256(g_evm_key, EVM_KEY_SIZE, buf, (uint32_t)buf_len, hmac_out);

    kfree(buf);
    return 0;
}

/*
 * evm_set_xattr — Set the security.evm xattr on a file.
 *
 * @path:  File path.
 *
 * Returns 0 on success, -1 on error.
 */
static int evm_set_xattr(const char *path)
{
    if (!path || !g_evm_key_set)
        return -EINVAL;

    uint8_t hmac[SHA256_DIGEST_SIZE];
    if (evm_compute_hmac(path, hmac) < 0)
        return -EIO;

    /* Write security.evm xattr — this is the HMAC of the protected xattrs */
    return vfs_setxattr(path, "security.evm", hmac, SHA256_DIGEST_SIZE);
}

/*
 * evm_verify_xattr — Verify the security.evm xattr on a file.
 *
 * @path:  File path.
 *
 * Returns:
 *   1  — HMAC matches (integrity good).
 *   0  — HMAC mismatch (tampered).
 *  -1  — Error (no key, no EVM xattr, I/O error).
 */
static int evm_verify_xattr(const char *path)
{
    if (!path || !g_evm_key_set)
        return -EINVAL;

    /* Read security.evm xattr */
    uint8_t stored_hmac[SHA256_DIGEST_SIZE];
    uint32_t stored_len = 0;

    int sret = vfs_getxattr(path, "security.evm", stored_hmac, sizeof(stored_hmac));
    if (sret < 0) {
        /* No EVM xattr — maybe not protected, or not yet initialized */
        return 0;  /* Treat as failure */
    }
    stored_len = (uint32_t)sret;

    if (stored_len != SHA256_DIGEST_SIZE)
        return 0;

    /* Compute expected HMAC */
    uint8_t computed[SHA256_DIGEST_SIZE];
    if (evm_compute_hmac(path, computed) < 0)
        return -EIO;

    /* Compare */
    if (memcmp(computed, stored_hmac, SHA256_DIGEST_SIZE) == 0)
        return 1;  /* Integrity verified */

    return 0;  /* HMAC mismatch */
}

/* Re-entrancy guard: evm_compute_hmac() reads the protected security.*
 * xattrs via vfs_getxattr(), which re-enters evm_verify_get().  This
 * flag short-circuits that re-entry so verification does not recurse. */
static int evm_verify_reentrant = 0;

/*
 * evm_verify_get — EVM integrity verification on xattr read (D312 item 9).
 *
 * Called from vfs_getxattr() before returning a protected security.*
 * xattr value.  Only files that actually carry a security.evm xattr are
 * verified; a tampered (HMAC mismatch) file is denied when enforcement
 * is enabled.  Un-protected files pass through untouched.
 *
 * Returns 0 to allow the read, -EACCES to deny, matching EVM policy.
 */
int evm_verify_get(const char *path, const char *xattr_name) {
    uint8_t present[SHA256_DIGEST_SIZE];
    int ret;

    if (!path || !xattr_name)
        return 0;

    /* Only the security.* xattrs EVM protects are subject to verification. */
    if (strncmp(xattr_name, "security.", 9) != 0)
        return 0;

    /* The security.evm attribute itself is never self-verified. */
    if (strcmp(xattr_name, "security.evm") == 0)
        return 0;

    if (!g_evm_initialized || !g_evm_key_set)
        return 0;

    /* Re-entrancy guard (see above). */
    if (evm_verify_reentrant)
        return 0;

    /* A file with no security.evm xattr is not protected — allow it. */
    if (vfs_getxattr(path, "security.evm", present, sizeof(present)) < 0)
        return 0;

    evm_verify_reentrant = 1;
    ret = evm_verify_xattr(path);
    evm_verify_reentrant = 0;

    if (ret == 1)
        return 0; /* integrity verified */

    if (g_evm_enforce) {
        kprintf("[EVM] getxattr %s denied on %s (integrity check failed)\n", xattr_name, path);
        return -EACCES;
    }

    kprintf("[EVM] Warning: integrity check failed reading %s on %s\n", xattr_name, path);
    return 0;
}

/*
 * evm_protect_file — Set or update the EVM HMAC on a file.
 * Should be called after any protected xattr changes.
 */
static int evm_protect_file(const char *path)
{
    return evm_set_xattr(path);
}

/*
 * evm_check_file — Verify EVM integrity before allowing file access.
 * Returns 1 if allowed, 0 if denied, -1 on error.
 */
static int evm_check_file(const char *path)
{
    int ret = evm_verify_xattr(path);

    if (ret == 1) {
        /* HMAC valid */
        return 1;
    }

    if (ret == 0) {
        /* HMAC invalid or missing */
        if (g_evm_enforce) {
            kprintf("[EVM] Denied access to %s (integrity check failed)\n", path);
            return 0;
        }
        /* Warn but allow */
        kprintf("[EVM] Warning: integrity check failed for %s\n", path);
        return 1;
    }

    return ret;
}

/*
 * evm_set_key — Set the EVM HMAC key.
 *
 * @key:   16-byte key.
 * @len:   Key length (must be EVM_KEY_SIZE).
 */
static int evm_set_key(const uint8_t *key, uint32_t len)
{
    if (!key || len < EVM_KEY_SIZE)
        return -EINVAL;

    memcpy(g_evm_key, key, EVM_KEY_SIZE);
    g_evm_key_set = 1;
    return 0;
}

/*
 * evm_has_key — Returns 1 if the EVM key is configured.
 */
static int evm_has_key(void)
{
    return g_evm_key_set;
}

/*
 * evm_set_enforce — Set enforcement mode.
 */
static void evm_set_enforce(int enforce)
{
    g_evm_enforce = enforce ? 1 : 0;
}

/*
 * evm_init — Initialize EVM.
 *
 * In a full implementation, the EVM key would be loaded from a TPM
 * or from the kernel keyring.  For now, we use a fixed test key.
 */
static void evm_init(void)
{
    if (g_evm_initialized) return;

    /* Use a default key (in production, load from TPM or keyring) */
    const uint8_t default_key[EVM_KEY_SIZE] = {
        0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
        0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10
    };
    memcpy(g_evm_key, default_key, EVM_KEY_SIZE);
    g_evm_key_set = 1;
    g_evm_enforce = 1;

    g_evm_initialized = 1;
    kprintf("[OK] EVM initialized (HMAC-SHA256, %s enforcement)\n",
            g_evm_enforce ? "with" : "without");
}
#include "module.h"
module_init(evm_init);

/* ── evm_verify — Verify EVM integrity on an xattr operation ──────── */
static int evm_verify(void *dentry, void *xattr_name, void *xattr_value, size_t xattr_len)
{
    if (!xattr_name) {
        kprintf("[EVM] evm_verify: NULL xattr_name\n");
        return -EINVAL;
    }
    if (*(const char *)xattr_name == '\0') {
        kprintf("[EVM] evm_verify: empty xattr_name\n");
        return -EINVAL;
    }
    if (!g_evm_initialized) {
        kprintf("[EVM] evm_verify: EVM not initialized\n");
        return -EAGAIN;
    }
    if (!dentry) {
        kprintf("[EVM] evm_verify: NULL dentry\n");
        return -EINVAL;
    }

    /* Ensure xattr_name starts with "security." prefix (EVM only protects
     * security-related xattrs).  Namespaced xattrs must begin with a known
     * prefix (security., system., trusted., user.). */
    const char *name = (const char *)xattr_name;
    static const char *const valid_prefixes[] = {
        "security.",
        "system.",
        "trusted.",
        "user.",
    };
    int valid = 0;
    for (int i = 0; i < 4; i++) {
        size_t plen = strlen(valid_prefixes[i]);
        if (strncmp(name, valid_prefixes[i], plen) == 0) {
            valid = 1;
            break;
        }
    }
    if (!valid) {
        kprintf("[EVM] evm_verify: invalid xattr name prefix: %s\n", name);
        return -EINVAL;
    }

    (void)xattr_value;
    (void)xattr_len;
    kprintf("[evm] evm_verify: not yet fully implemented\n");
    return 0;
}
/* ── Stub: evm_update ─────────────────────────────── */
static int evm_update(void *dentry)
{
    (void)dentry;
    kprintf("[evm] evm_update: not yet implemented\n");
    return 0;
}
