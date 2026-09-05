/*
 * ima_appraise.c — IMA Appraisal
 *
 * Verifies file content integrity by comparing a SHA-256 hash of
 * the file's contents against the expected hash stored in the
 * security.ima extended attribute.  If the hashes match, the file
 * is considered integral; otherwise access is denied (when IMA
 * appraisal enforcement is enabled).
 *
 * This integrates with the IMA policy engine (ima_policy.c) and
 * the existing ima.c measurement subsystem.
 *
 * Item S101 — IMA appraisal
 */

#include "audit.h"
#include "errno.h"
#include "heap.h"
#include "ima.h"
#include "printf.h"
#include "sha256.h"
#include "string.h"
#include "types.h"
#include "vfs.h"
#include "xattr.h"

/* ── Enforcement policy mode ─────────────────────────────────────── */

/* Appraisal policy: 0=off, 1=fix, 2=log, 3=enforce.
 * Default is enforce (deny on mismatch). */
static int ima_appraise_mode = IMA_APPRAISE_ENFORCE;

const char *ima_appraise_mode_name(int mode) {
    switch (mode) {
    case IMA_APPRAISE_OFF:
        return "off";
    case IMA_APPRAISE_FIX:
        return "fix";
    case IMA_APPRAISE_LOG:
        return "log";
    case IMA_APPRAISE_ENFORCE:
        return "enforce";
    default:
        break;
    }
    return "off";
}

int ima_appraise_set_mode(const char *name, int len) {
    static const char off[] = "off";
    static const char fix[] = "fix";
    static const char log[] = "log";
    static const char enforce[] = "enforce";

    if (!name || len <= 0)
        return -EINVAL;

    if (len == (int)sizeof(off) - 1 && strncmp(name, off, (size_t)len) == 0)
        ima_appraise_mode = IMA_APPRAISE_OFF;
    else if (len == (int)sizeof(fix) - 1 && strncmp(name, fix, (size_t)len) == 0)
        ima_appraise_mode = IMA_APPRAISE_FIX;
    else if (len == (int)sizeof(log) - 1 && strncmp(name, log, (size_t)len) == 0)
        ima_appraise_mode = IMA_APPRAISE_LOG;
    else if (len == (int)sizeof(enforce) - 1 && strncmp(name, enforce, (size_t)len) == 0)
        ima_appraise_mode = IMA_APPRAISE_ENFORCE;
    else
        return -EINVAL;

    return 0;
}

int ima_appraise_get_mode(void) {
    return ima_appraise_mode;
}

/* Hex-encode a file hash for storage in the security.ima xattr (the
 * format ima.c's ima_appraise reads back: 64 lowercase hex chars). */
static void ima_appraise_hash_to_hex(const uint8_t *hash, char *hex, int hex_len) {
    static const char hex_chars[] = "0123456789abcdef";
    int i;
    for (i = 0; i < SHA256_DIGEST_SIZE && (i * 2 + 1) < hex_len; i++) {
        hex[i * 2] = hex_chars[(hash[i] >> 4) & 0x0F];
        hex[i * 2 + 1] = hex_chars[hash[i] & 0x0F];
    }
    hex[hex_len - 1] = '\0';
}

int ima_appraise_eval(int match, const char *path, const uint8_t *hash) {
    char abuf[256];
    const char *pname = path ? path : "?";

    if (match) {
        /* integral — audit the pass and allow access. */
        snprintf(abuf, sizeof(abuf), "ima appraise=ok path=%s", pname);
        audit_log(abuf);
        return 0;
    }

    /* Hash mismatch or missing/invalid xattr. */
    switch (ima_appraise_mode) {
    case IMA_APPRAISE_FIX:
        /* Allow access and repair: rewrite security.ima with the correct
         * hash so the file is integral on the next appraisal. */
        kprintf("[IMA-APPRAISE] Fix: %s (rewriting security.ima)\n", pname);
        if (path && hash) {
            char hex[SHA256_DIGEST_SIZE * 2 + 1];
            ima_appraise_hash_to_hex(hash, hex, (int)sizeof(hex));
            (void)vfs_setxattr(path, "security.ima", hex, SHA256_DIGEST_SIZE * 2);
        }
        snprintf(abuf, sizeof(abuf), "ima appraise=fix repaired path=%s", pname);
        audit_log(abuf);
        return 0;
    case IMA_APPRAISE_LOG:
        /* Allow access but log the failure. */
        kprintf("[IMA-APPRAISE] LOG: %s failed appraisal (permissive)\n", pname);
        snprintf(abuf, sizeof(abuf), "ima appraise=log-fail path=%s", pname);
        audit_log(abuf);
        return 0;
    case IMA_APPRAISE_ENFORCE:
    default:
        kprintf("[IMA-APPRAISE] Denied access to %s (appraisal failed)\n", pname);
        /* Emit a proper audit denial record in addition to the event log. */
        audit_log_denial("kernel", pname, "ima_appraise");
        return -EACCES;
    }
}

/*
 * ima_appraise_file — Verify a file's integrity.
 *
 * Computes SHA-256 hash of the file contents and compares against
 * the value in the security.ima extended attribute.
 *
 * @path:  Full path to the file.
 *
 * Returns:
 *   1  — File is integral (hash matches or no xattr but fix mode)
 *   0  — File fails appraisal (hash mismatch)
 *  -1  — Error (file not found, no xattr, etc.)
 */
static int ima_appraise_file(const char *path)
{
    if (!path)
        return -EINVAL;

    /* Get the security.ima xattr */
    uint8_t xattr_hash[SHA256_DIGEST_SIZE];
    int ret = vfs_getxattr(path, "security.ima", xattr_hash, sizeof(xattr_hash));

    /* Compute hash of file contents */
    struct vfs_stat st;
    if (vfs_stat(path, &st) < 0)
        return -EIO;

    if (st.type != 1) /* Regular file only */
        return -EINVAL;

    uint8_t computed[SHA256_DIGEST_SIZE];

    if (st.size == 0) {
        /* Empty file: hash of zero-length input */
        sha256_hash(computed, (const uint8_t *)"", 0);
    } else {
        /* Allocate buffer and read file */
        void *buf = kmalloc((size_t)st.size);
        if (!buf)
            return -ENOMEM;

        uint32_t bytes_read = 0;
        if (vfs_read(path, buf, (uint32_t)st.size, &bytes_read) < 0 ||
            bytes_read != (uint32_t)st.size) {
            kfree(buf);
            return -EIO;
        }

        sha256_hash(computed, (const uint8_t *)buf, (size_t)bytes_read);
        kfree(buf);
    }

    /* Compare hashes; a missing xattr counts as a mismatch so the
     * policy mode (enforce/log/fix) decides the outcome. */
    int match = (ret >= 0 && memcmp(computed, xattr_hash, SHA256_DIGEST_SIZE) == 0);

    /* Apply policy mode: eval==0 allows access (return 1), deny otherwise. */
    if (ima_appraise_eval(match, path, computed) == 0)
        return 1;
    return 0;
}

/*
 * ima_appraise_init — Initialize the IMA appraisal subsystem.
 * Default policy mode is enforce (deny on mismatch).
 */
static void ima_appraise_init(void)
{
    ima_appraise_mode = IMA_APPRAISE_ENFORCE;
    kprintf("[OK] IMA appraisal initialized (mode=%s)\n",
            ima_appraise_mode_name(ima_appraise_mode));
}
#include "module.h"
module_init(ima_appraise_init);

/* ── Stub: ima_appraise_measurement ─────────────────────────────── */
static int ima_appraise_measurement(void *inode, void *file, void *xattr_value, int xattr_len)
{
    (void)inode;
    (void)file;
    (void)xattr_value;
    (void)xattr_len;
    kprintf("[ima] ima_appraise_measurement: not yet implemented\n");
    return 0;
}
/* ── Stub: ima_appraise_signature ─────────────────────────────── */
static int ima_appraise_signature(void *inode, void *file, void *sig)
{
    (void)inode;
    (void)file;
    (void)sig;
    kprintf("[ima] ima_appraise_signature: not yet implemented\n");
    return 0;
}
