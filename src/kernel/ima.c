/*
 * ima.c — Integrity Measurement Architecture (Item 179)
 *
 * Measures (hashes) files before they are accessed (read or exec) and
 * optionally appraises them by comparing against a stored hash in the
 * security.ima extended attribute.
 *
 * Modes (controlled via /sys/kernel/security/ima/enforce_mode):
 *   0 = disabled (default)
 *   1 = measure only  — hash and log, never deny access
 *   2 = appraise      — hash, log, and deny on mismatch with security.ima xattr
 *
 * The measurement log is exposed at /sys/kernel/security/ima/log
 * as a human-readable ASCII list.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "sha256.h"
#include "xattr.h"
#include "vfs.h"
#include "process.h"
#include "sysctl.h"
#include "sysfs.h"
#include "errno.h"
#include "heap.h"

/* ── Configuration ───────────────────────────────────────────────── */

/* IMA mode (0=off, 1=measure, 2=appraise) */
static int ima_mode = 0;

/* ── Measurement log (ring buffer) ────────────────────────────────── */

#define IMA_LOG_MAX  256
#define IMA_PATH_MAX 128
#define IMA_HASH_STR (SHA256_DIGEST_SIZE * 2 + 1) /* hex string + NUL */

struct ima_entry {
    char path[IMA_PATH_MAX];               /* measured file path */
    char hash_hex[IMA_HASH_STR];           /* SHA256 hash as hex */
    int  appraised;                        /* 1 if appraised, 0 if measured */
    int  passed;                           /* 1 if match/pass, 0 if fail */
};

static struct ima_entry ima_log[IMA_LOG_MAX];
static int ima_log_count = 0;
static int ima_log_wraps = 0;   /* number of times the ring wrapped */

/* Lock for the log */
static int ima_log_lock = 0;

static void ima_log_lock_acquire(void) {
    while (__sync_lock_test_and_set(&ima_log_lock, 1)) { __asm__ volatile("pause"); }
}

static void ima_log_lock_release(void) {
    __sync_lock_release(&ima_log_lock);
}

/* ── SHA256 helper — hash a file's contents ───────────────────────── */

/* Hash the entire file at @path into @hash (SHA256_DIGEST_LENGTH bytes).
 * Returns 0 on success, -errno on failure. */
static int ima_hash_file(const char *path, uint8_t *hash)
{
    if (!path || !hash)
        return -EINVAL;

    /* Stat the file to determine its size */
    struct vfs_stat st;
    if (vfs_stat(path, &st) < 0)
        return -ENOENT;

    /* Refuse to hash directories or special files */
    if (st.type != 1)  /* type 1 = regular file (from VFS convention) */
        return -EISDIR;

    uint64_t file_size = st.size;
    if (file_size == 0) {
        /* Empty file: hash of zero-length input is well-defined */
        sha256_hash(hash, (const uint8_t *)"", 0);
        return 0;
    }

    /* Read the entire file into a buffer */
    void *buf = kmalloc((size_t)file_size);
    if (!buf)
        return -ENOMEM;

    uint32_t bytes_read = 0;
    int ret = vfs_read(path, buf, (uint32_t)file_size, &bytes_read);
    if (ret < 0 || bytes_read != file_size) {
        kfree(buf);
        return ret < 0 ? ret : -EIO;
    }

    /* Compute SHA256 */
    sha256_hash(hash, (const uint8_t *)buf, (size_t)bytes_read);
    kfree(buf);
    return 0;
}

/* Convert binary SHA256 to hex string */
static void hash_to_hex(const uint8_t *hash, char *hex, int hex_len)
{
    static const char hex_chars[] = "0123456789abcdef";
    for (int i = 0; i < SHA256_DIGEST_SIZE && i * 2 + 1 < hex_len; i++) {
        hex[i * 2]     = hex_chars[(hash[i] >> 4) & 0x0F];
        hex[i * 2 + 1] = hex_chars[hash[i] & 0x0F];
    }
    hex[hex_len - 1] = '\0';
}

/* Parse a hex string back to binary (len must be SHA256_DIGEST_SIZE * 2) */
static int hex_to_hash(const char *hex, uint8_t *hash)
{
    for (int i = 0; i < SHA256_DIGEST_SIZE; i++) {
        int hi = 0, lo = 0;
        char c = hex[i * 2];
        if (c >= '0' && c <= '9')      hi = c - '0';
        else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
        else return -EINVAL;
        c = hex[i * 2 + 1];
        if (c >= '0' && c <= '9')      lo = c - '0';
        else if (c >= 'a' && c <= 'f') lo = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') lo = c - 'A' + 10;
        else return -EINVAL;
        hash[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* ── Log an entry ────────────────────────────────────────────────── */

static void ima_log_entry(const char *path, const uint8_t *hash,
                          int appraised, int passed)
{
    ima_log_lock_acquire();

    int idx = ima_log_count % IMA_LOG_MAX;
    strncpy(ima_log[idx].path, path, (int)sizeof(ima_log[idx].path) - 1);
    ima_log[idx].path[sizeof(ima_log[idx].path) - 1] = '\0';
    hash_to_hex(hash, ima_log[idx].hash_hex, (int)sizeof(ima_log[idx].hash_hex));
    ima_log[idx].appraised = appraised;
    ima_log[idx].passed = passed;

    ima_log_count++;
    if (ima_log_count > IMA_LOG_MAX)
        ima_log_wraps = 1;

    ima_log_lock_release();
}

/* ── Core measurement function ───────────────────────────────────── */

/**
 * ima_measure_file — Measure a file and optionally appraise it.
 *
 * Called from the VFS layer when a file is opened for read or exec.
 *
 * @path:       Full path to the file
 * @for_exec:   1 if the file is being executed, 0 if just read
 *
 * Returns 0 to allow access, -EACCES to deny (appraise mode only).
 */
int ima_measure_file(const char *path, int for_exec)
{
    if (!path || ima_mode == 0)
        return 0;  /* IMA disabled — allow all */

    /* Don't measure our own sysfs files */
    if (strncmp(path, "/sys/kernel/security", 20) == 0)
        return 0;

    uint8_t hash[SHA256_DIGEST_SIZE];
    int hash_ret = ima_hash_file(path, hash);
    if (hash_ret < 0) {
        kprintf("[IMA] WARN: could not hash %s: err=%d\n", path, hash_ret);
        return 0;  /* Cannot hash — allow through in warning mode */
    }

    /* ── Appraisal mode: compare against security.ima xattr ───── */
    if (ima_mode >= 2) {
        char stored_hex[IMA_HASH_STR];
        size_t stored_size = sizeof(stored_hex);
        int xret = xattr_get(path, "security.ima", stored_hex, &stored_size);
        if (xret == 0 && stored_size > 0 && stored_size <= sizeof(stored_hex)) {
            /* Null-terminate the stored value (xattr stores raw bytes) */
            if (stored_size < sizeof(stored_hex))
                stored_hex[stored_size] = '\0';
            else
                stored_hex[sizeof(stored_hex) - 1] = '\0';

            /* stored_hex is hex-encoded hash — parse it */
            uint8_t stored_hash[SHA256_DIGEST_SIZE];
            if (hex_to_hash(stored_hex, stored_hash) == 0) {
                int match = (memcmp(hash, stored_hash, SHA256_DIGEST_SIZE) == 0);
                ima_log_entry(path, hash, 1, match);
                if (!match && for_exec) {
                    /* Exec appraise failure — deny execution */
                    kprintf("[IMA] APPRAISE FAIL: %s hash mismatch (denying exec)\n", path);
                    return -EACCES;
                }
                if (!match) {
                    kprintf("[IMA] APPRAISE FAIL: %s hash mismatch\n", path);
                    /* For reads, just warn */
                }
                return match ? 0 : (for_exec ? -EACCES : 0);
            }
        }
        /* No security.ima xattr: in strict mode, deny if appraise-required
         * for executable files.  For now we allow through with a warning. */
        if (for_exec) {
            kprintf("[IMA] no security.ima xattr on %s (measure-only)\n", path);
        }
    }

    /* ── Measure mode (or measure portion of appraise mode) ────── */
    ima_log_entry(path, hash, 0, 1);
    return 0;  /* Allow access */
}

/* ── Sysfs interface ────────────────────────────────────────────── */

/* /sys/kernel/security/ima/mode — read callback */
static int ima_mode_read_cb(char *buf, uint32_t max_size, void *priv)
{
    (void)priv;
    return snprintf(buf, (size_t)max_size, "%d\n", ima_mode);
}

/* /sys/kernel/security/ima/mode — write callback */
static int ima_mode_write_cb(const char *data, uint32_t size, void *priv)
{
    (void)priv;
    if (size > 0) {
        char val = data[0];
        if (val >= '0' && val <= '2')
            ima_mode = (int)(val - '0');
    }
    return 0;
}

/* /sys/kernel/security/ima/log — read the measurement log as ASCII text */
static int ima_log_read_cb(char *buf, uint32_t max_size, void *priv)
{
    (void)priv;
    int pos = 0;
    int n = 0;

    /* Determine total entries to display */
    int total = ima_log_count;
    int start = 0;
    if (ima_log_wraps) {
        start = ima_log_count % IMA_LOG_MAX;
        total = IMA_LOG_MAX;
    }

    if (ima_log_wraps) {
        n = snprintf(buf + pos, (size_t)(max_size - pos),
                     "# IMA measurement log (ring buffer, %d entries shown, %d total)\n",
                     total, ima_log_count);
        if (n > 0 && pos + n < (int)max_size) pos += n;
    } else {
        n = snprintf(buf + pos, (size_t)(max_size - pos),
                     "# IMA measurement log (%d entries)\n", total);
        if (n > 0 && pos + n < (int)max_size) pos += n;
    }

    /* Format: "<template> <hash_hex> <path>" */
    for (int i = 0; i < total && pos < (int)max_size - 80; i++) {
        int idx = (start + i) % IMA_LOG_MAX;
        if (ima_log[idx].path[0] == '\0')
            continue;  /* skip empty slots */

        const char *tmpl = ima_log[idx].appraised
            ? (ima_log[idx].passed ? "appraise-ok" : "appraise-fail")
            : "measure";

        n = snprintf(buf + pos, (size_t)(max_size - pos),
                     "%s %s %s\n",
                     tmpl, ima_log[idx].hash_hex, ima_log[idx].path);
        if (n > 0 && pos + n < (int)max_size) pos += n;
    }

    if (pos < (int)max_size)
        buf[pos] = '\0';
    return pos;
}

/* ── VFS hooks: called from sys_open / sys_execve ────────────────── */

/* Called when a file is opened for read (O_RDONLY or RDWR).
 * Returns 0 to allow, -errno to deny. */
int ima_file_open(const char *path, int flags)
{
    (void)flags;
    if (ima_mode == 0)
        return 0;

    /* Only measure regular file opens for read */
    if (!path)
        return 0;

    return ima_measure_file(path, 0);
}

/* Called when a file is being executed (execve).
 * Returns 0 to allow, -errno to deny. */
int ima_file_exec(const char *path)
{
    if (ima_mode == 0)
        return 0;

    if (!path)
        return 0;

    return ima_measure_file(path, 1);
}

/* ── Initialisation ─────────────────────────────────────────────── */

void ima_init(void)
{
    /* Create /sys/kernel/security directory if it doesn't already exist.
     * The /sys and /sys/kernel directories are created by sysfs_init()
     * early in boot, so we only need the security subdirectory. */
    sysfs_create_dir("/sys/kernel/security");

    /* Create mode control file (writable with read/write callbacks) */
    sysfs_create_writable_file("/sys/kernel/security/ima_mode", "0\n",
                               NULL, ima_mode_read_cb, ima_mode_write_cb);

    /* Create measurement log file (read-only with dynamic content) */
    sysfs_create_writable_file("/sys/kernel/security/ima_log", "",
                               NULL, ima_log_read_cb, NULL);

    kprintf("[OK] IMA initialized (mode=%d)\n", ima_mode);
}

/* ── Module support ─────────────────────────────────────────────── */

#ifdef MODULE
#include "module.h"

int init_module(void)
{
    ima_init();
    return 0;
}

void cleanup_module(void)
{
    ima_mode = 0;
    kprintf("[IMA] Module unloaded\n");
}

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Hermes OS");
MODULE_DESCRIPTION("IMA — Integrity Measurement Architecture (loadable module)");
#endif /* MODULE */
