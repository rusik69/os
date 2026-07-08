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

#include "types.h"
#include "string.h"
#include "printf.h"
#include "sha256.h"
#include "xattr.h"
#include "vfs.h"
#include "errno.h"
#include "heap.h"

/* ── Enforcement mode ────────────────────────────────────────────── */
static int ima_appraise_enforce = 1;   /* 1 = deny on mismatch */

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

    if (ret < 0) {
        /* No extended attribute — cannot appraise */
        kprintf("[IMA-APPRAISE] No security.ima xattr on %s\n", path);

        if (ima_appraise_enforce) {
            kprintf("[IMA-APPRAISE] Denied access to %s (no hash)\n", path);
            return 0;
        }
        /* In permissive mode, allow without xattr */
        return 1;
    }

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

    /* Compare hashes */
    if (memcmp(computed, xattr_hash, SHA256_DIGEST_SIZE) == 0) {
        /* Hash matches — file is integral */
        return 1;
    }

    /* Hash mismatch */
    kprintf("[IMA-APPRAISE] Hash mismatch on %s\n", path);

    if (ima_appraise_enforce) {
        kprintf("[IMA-APPRAISE] Denied access to %s (hash mismatch)\n", path);
        return 0;
    }

    return 0;
}

/*
 * ima_appraise_set_enforce — Set enforcement mode.
 * @enforce: 1 = deny on mismatch, 0 = warn only.
 */
static void ima_appraise_set_enforce(int enforce)
{
    ima_appraise_enforce = enforce ? 1 : 0;
}

/*
 * ima_appraise_get_enforce — Get current enforcement mode.
 */
static int ima_appraise_get_enforce(void)
{
    return ima_appraise_enforce;
}

/*
 * ima_appraise_init — Initialize the IMA appraisal subsystem.
 */
static void ima_appraise_init(void)
{
    ima_appraise_enforce = 1;
    kprintf("[OK] IMA appraisal initialized (%s enforcement)\n",
            ima_appraise_enforce ? "with" : "without");
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
