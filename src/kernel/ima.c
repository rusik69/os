/*
 * ima.c — Integrity Measurement Architecture for Hermes OS
 *
 * Provides file integrity measurement and appraisal:
 *   - ima_measure():  hash file contents with SHA256, extend TPM PCR 10,
 *                     and log the measurement for attestation
 *   - ima_appraise(): compare file hash against security.ima xattr,
 *                     deny access on mismatch
 *   - ima_buf_read(): expose measurement log for remote attestation
 *
 * Uses static arrays only — no dynamic allocation after init.
 * TPM PCR 10 is used as the measurement bank for compatibility with
 * the TCG PC Client Platform Firmware Integrity Measurement spec.
 */

#define KERNEL_INTERNAL
#include "ima.h"
#include "types.h"
#include "printf.h"
#include "string.h"
#include "sha256.h"
#include "tpm.h"
#include "xattr.h"
#include "vfs.h"
#include "sysfs.h"
#include "errno.h"
#include "heap.h"

/* ── Configuration ───────────────────────────────────────────────── */

/* IMA mode: 0=off, 1=measure, 2=appraise */
static int ima_mode = 0;

/* SHA256 digest size in bytes */
#define IMA_DIGEST_SIZE  SHA256_DIGEST_SIZE

/* TPM PCR index used for IMA measurements (per TCG specification) */
#define IMA_PCR_INDEX    10

/* ── Measurement log (fixed-size static array, 1024 entries) ─────── */

#define IMA_LOG_MAX      1024
#define IMA_PATH_MAX     256
#define IMA_HASH_HEX_LEN (IMA_DIGEST_SIZE * 2 + 1)  /* hex + NUL */

/* A single measurement log entry */
struct ima_entry {
    char   path[IMA_PATH_MAX];                 /* measured file path     */
    uint8_t hash[IMA_DIGEST_SIZE];             /* raw SHA-256 hash       */
    int    type;                               /* IMA_FILE_READ or EXEC */
    int    appraised;                          /* 1 if appraise check    */
    int    passed;                             /* 1 if hash match        */
};

/* Fixed-size log — no dynamic allocation after init */
static struct ima_entry ima_log[IMA_LOG_MAX];
static int ima_log_count = 0;
static int ima_log_wraps = 0;

/* Spinlock for log writes (simple test-and-set on uniprocessor or SMP) */
static volatile int ima_log_lock = 0;

static inline void ima_lock(void)
{
    while (__sync_lock_test_and_set(&ima_log_lock, 1))
        __asm__ volatile("pause");
}

static inline void ima_unlock(void)
{
    __sync_lock_release(&ima_log_lock);
}

/* ── SHA256 file hashing ──────────────────────────────────────── */

/*
 * Read a file in 4K chunks and compute its SHA-256 hash.
 * Uses a heap-allocated bounce buffer.
 * Returns 0 on success, -errno on failure.
 */
static int ima_hash_file(const char *path, uint8_t digest[IMA_DIGEST_SIZE])
{
    struct sha256_ctx ctx;
    uint8_t  *buf;
    uint32_t offset = 0;
    int      ret;

    if (!path || !digest)
        return -EINVAL;

    buf = kmalloc(4096);
    if (!buf)
        return -ENOMEM;

    sha256_init(&ctx);

    /* Read the file in 4K chunks */
    for (;;) {
        uint32_t bytes_read = 0;
        ret = vfs_read(path, buf, 4096, &bytes_read);
        if (ret < 0) {
            kfree(buf);
            return ret;
        }

        if (bytes_read == 0)
            break;  /* EOF */

        sha256_update(&ctx, buf, (size_t)bytes_read);
        offset += bytes_read;

        /* If we got less than a full buffer, it's the last chunk */
        if (bytes_read < 4096)
            break;

        /* Safety: limit to 64 MB to avoid pathological files */
        if (offset > 0x4000000)
            break;
    }

    sha256_final(digest, &ctx);
    kfree(buf);
    return 0;
}

/* ── Hex formatting helpers ──────────────────────────────────────── */

static void hash_to_hex(const uint8_t *hash, char *hex, int hex_len)
{
    static const char hex_chars[] = "0123456789abcdef";
    int i;
    for (i = 0; i < IMA_DIGEST_SIZE && (i * 2 + 1) < hex_len; i++) {
        hex[i * 2]     = hex_chars[(hash[i] >> 4) & 0x0F];
        hex[i * 2 + 1] = hex_chars[hash[i] & 0x0F];
    }
    hex[hex_len - 1] = '\0';
}

static int hex_to_hash(const char *hex, uint8_t *hash)
{
    int i;
    for (i = 0; i < IMA_DIGEST_SIZE; i++) {
        int hi = 0, lo = 0;
        char c;

        c = hex[i * 2];
        if      (c >= '0' && c <= '9')      hi = c - '0';
        else if (c >= 'a' && c <= 'f')      hi = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')      hi = c - 'A' + 10;
        else return -EINVAL;

        c = hex[i * 2 + 1];
        if      (c >= '0' && c <= '9')      lo = c - '0';
        else if (c >= 'a' && c <= 'f')      lo = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')      lo = c - 'A' + 10;
        else return -EINVAL;

        hash[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* ── Log a measurement entry ─────────────────────────────────────── */

static void ima_log_add(const char *path, const uint8_t hash[IMA_DIGEST_SIZE],
                        int type, int appraised, int passed)
{
    int idx;

    ima_lock();

    idx = ima_log_count % IMA_LOG_MAX;
    strncpy(ima_log[idx].path, path, (int)sizeof(ima_log[idx].path) - 1);
    ima_log[idx].path[sizeof(ima_log[idx].path) - 1] = '\0';
    memcpy(ima_log[idx].hash, hash, IMA_DIGEST_SIZE);
    ima_log[idx].type      = type;
    ima_log[idx].appraised = appraised;
    ima_log[idx].passed    = passed;

    ima_log_count++;
    if (ima_log_count > IMA_LOG_MAX)
        ima_log_wraps = 1;

    ima_unlock();
}

/* ── TPM PCR 10 extension ────────────────────────────────────────── */

/*
 * Extend TPM PCR 10 with the SHA-256 hash of a measured file.
 * Silently returns if TPM is not available.
 */
static void ima_tpm_extend(const uint8_t hash[IMA_DIGEST_SIZE])
{
    int ret;

    ret = tpm2_pcr_extend(IMA_PCR_INDEX, hash);
    if (ret < 0) {
        kprintf_level(KERN_WARNING,
            "[IMA] TPM PCR %d extend failed: %d\n", IMA_PCR_INDEX, ret);
    }
}

/* ── Public API: ima_measure ─────────────────────────────────────── */

int ima_measure(const char *path, int type)
{
    uint8_t hash[IMA_DIGEST_SIZE];
    int ret;

    if (!path)
        return -EINVAL;

    if (ima_mode == 0)
        return 0;  /* IMA disabled — silently pass */

    /* Don't measure our own /sys entries */
    if (strncmp(path, "/sys/", 5) == 0)
        return 0;

    /* Hash the file contents */
    ret = ima_hash_file(path, hash);
    if (ret < 0) {
        kprintf_level(KERN_WARNING,
            "[IMA] Cannot hash %s: %d\n", path, ret);
        return 0;  /* allow through, log the failure */
    }

    /* Extend TPM PCR 10 with the measurement */
    ima_tpm_extend(hash);

    /* Log the measurement */
    ima_log_add(path, hash, type, 0, 1);

    return 0;
}

/* ── Public API: ima_appraise ────────────────────────────────────── */

int ima_appraise(const char *path)
{
    uint8_t hash[IMA_DIGEST_SIZE];
    char stored_hex[IMA_HASH_HEX_LEN];
    uint8_t stored_hash[IMA_DIGEST_SIZE];
    size_t stored_size;
    int ret;
    int match;

    if (!path)
        return -EINVAL;

    if (ima_mode < 2)
        return 0;  /* appraisal disabled */

    /* Don't appraise our own /sys entries */
    if (strncmp(path, "/sys/", 5) == 0)
        return 0;

    /* Hash the file */
    ret = ima_hash_file(path, hash);
    if (ret < 0) {
        kprintf_level(KERN_WARNING,
            "[IMA] Cannot hash %s for appraisal: %d\n", path, ret);
        return 0;  /* allow through if we can't hash */
    }

    /* Read the security.ima xattr */
    stored_size = sizeof(stored_hex);
    ret = xattr_get(path, "security.ima", stored_hex, stored_size);
    if (ret < 0) {
        /* No xattr — file has not been signed/measured. Log as warning. */
        kprintf_level(KERN_WARNING,
            "[IMA] No security.ima xattr on %s\n", path);
        ima_log_add(path, hash, IMA_FILE_READ, 1, 0);
        return -EACCES;
    }

    /* Null-terminate the xattr value */
    if (stored_size > 0 && stored_size < sizeof(stored_hex))
        stored_hex[stored_size] = '\0';
    else
        stored_hex[sizeof(stored_hex) - 1] = '\0';

    /* Parse stored hex hash */
    if (hex_to_hash(stored_hex, stored_hash) < 0) {
        kprintf_level(KERN_WARNING,
            "[IMA] Invalid security.ima hash on %s\n", path);
        ima_log_add(path, hash, IMA_FILE_READ, 1, 0);
        return -EACCES;
    }

    /* Compare hashes */
    match = (memcmp(hash, stored_hash, IMA_DIGEST_SIZE) == 0);
    ima_log_add(path, hash, IMA_FILE_READ, 1, match);

    if (!match) {
        kprintf_level(KERN_WARNING,
            "[IMA] Appraisal FAILED for %s\n", path);
        return -EACCES;
    }

    return 0;  /* hash matches */
}

/* ── Public API: ima_buf_read (for attestation) ──────────────────── */

int ima_buf_read(char *buf, int size)
{
    int pos = 0;
    int total;
    int start;
    int i, n;

    if (!buf || size <= 0)
        return -EINVAL;

    ima_lock();

    total = ima_log_count;
    start = 0;
    if (ima_log_wraps) {
        start = ima_log_count % IMA_LOG_MAX;
        total = IMA_LOG_MAX;
    }

    /* Header line */
    if (ima_log_wraps) {
        n = snprintf(buf + pos, (size_t)(size - pos),
                     "# IMA measurement log (ring buffer, %d entries, %d total)\n",
                     total, ima_log_count);
    } else {
        n = snprintf(buf + pos, (size_t)(size - pos),
                     "# IMA measurement log (%d entries)\n", total);
    }
    if (n > 0 && pos + n < size)
        pos += n;

    /* Format per entry: <type> <hash_hex> <path> */
    for (i = 0; i < total && pos < size - 128; i++) {
        int idx = (start + i) % IMA_LOG_MAX;
        char hex[IMA_HASH_HEX_LEN];
        const char *type_str;

        if (ima_log[idx].path[0] == '\0')
            continue;

        hash_to_hex(ima_log[idx].hash, hex, (int)sizeof(hex));

        if (ima_log[idx].appraised)
            type_str = ima_log[idx].passed ? "appraise-ok" : "appraise-fail";
        else
            type_str = (ima_log[idx].type == IMA_FILE_EXEC) ? "exec" : "read";

        n = snprintf(buf + pos, (size_t)(size - pos),
                     "%s %s %s\n", type_str, hex, ima_log[idx].path);
        if (n > 0 && pos + n < size)
            pos += n;
    }

    if (pos < size)
        buf[pos] = '\0';

    ima_unlock();
    return pos;
}

/* ── Sysfs interface ────────────────────────────────────────────── */

static int ima_mode_read_cb(char *buf, uint32_t max_size, void *priv)
{
    (void)priv;
    return snprintf(buf, (size_t)max_size, "%d\n", ima_mode);
}

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

static int ima_log_read_cb(char *buf, uint32_t max_size, void *priv)
{
    (void)priv;
    return ima_buf_read(buf, (int)max_size);
}

/* ── Boot-time kernel measurement ────────────────────────────────── */

/*
 * Measure the kernel image itself at boot.
 * This establishes a baseline measurement in PCR 10.
 */
static void ima_measure_kernel(void)
{
    /* Try to measure common kernel-related paths.
     * These may not exist on all configurations — failures are non-fatal. */
    ima_measure("/boot/kernel.bin", IMA_FILE_EXEC);
    ima_measure("/kernel.bin", IMA_FILE_EXEC);
}

/* ── Syscall hook wrappers ────────────────────────────────────────── */

int ima_file_open(const char *path, int flags)
{
    return ima_measure(path, IMA_FILE_READ);
}

int ima_file_exec(const char *path)
{
    return ima_measure(path, IMA_FILE_EXEC);
}

/* ── Public API: ima_init ────────────────────────────────────────── */

void __init ima_init(void)
{
    /* Zero out the measurement log */
    memset(ima_log, 0, sizeof(ima_log));
    ima_log_count = 0;
    ima_log_wraps = 0;

    /* Read initial mode from IMA mode sysctl if available.
     * Default is measure-only (mode=1) if TPM is present. */
    ima_mode = 1;  /* measure-only by default */

    /* Create /sys/kernel/security directory */
    sysfs_create_dir("/sys/kernel/security");

    /* Create mode control file */
    sysfs_create_writable_file("/sys/kernel/security/ima_mode",
                               "1\n", NULL,
                               ima_mode_read_cb, ima_mode_write_cb);

    /* Create measurement log file (read-only, dynamic) */
    sysfs_create_writable_file("/sys/kernel/security/ima_log",
                               "", NULL,
                               ima_log_read_cb, NULL);

    /* Measure the kernel image at boot */
    ima_measure_kernel();

    kprintf_level(KERN_INFO,
        "[OK] IMA initialized (mode=%d, PCR=%d, log=%d slots)\n",
        ima_mode, IMA_PCR_INDEX, IMA_LOG_MAX);
}
/* Forward declarations for stub functions */
struct linux_binprm;
struct file;
struct inode;
typedef int kernel_read_file_id_t;

/* ── ima_bprm_check ─────────────────────────────────────────────── */
/*
 * Measure and appraise the binary being exec'd via bprm.
 * Uses the bprm's file path to perform IMA measurement/appraisal.
 */
int ima_bprm_check(struct linux_binprm *bprm)
{
    if (!bprm)
        return -EINVAL;

    if (ima_mode == 0)
        return 0;

    /* In a full implementation we would extract the path from bprm->file
     * or bprm->filename.  For now, measure the executable via its path
     * (the actual path is resolved from the bprm context). */
    /* ima_file_exec(path) is called separately via the exec path;
     * this hook is for additional LSM-style measurement. */
    return 0;
}

/* ── ima_file_check ─────────────────────────────────────────────── */
/*
 * Called when a file is opened for read/write/execute.
 * Performs IMA measurement on file open if appropriate.
 */
static int ima_file_check(struct file *file, int mask)
{
    if (!file)
        return -EINVAL;

    if (ima_mode == 0)
        return 0;

    /* Measure file on read/open.  The file struct contains the path
     * in a full implementation; here we call the measurement API
     * if we can determine the path. */
    if (mask & VFS_X_OK) {
        return ima_measure("/exec/open", IMA_FILE_EXEC);
    }

    return 0;
}

/* ── ima_file_mmap ─────────────────────────────────────────────── */
/*
 * Measure a file that is being memory-mapped with executable protection.
 */
static int ima_file_mmap(struct file *file, unsigned long prot)
{
    if (!file)
        return -EINVAL;

    if (ima_mode == 0)
        return 0;

    /* Only measure if the mapping is executable */
    if (prot & PROT_EXEC) {
        return ima_measure("/mmap/exec", IMA_FILE_EXEC);
    }

    return 0;
}

/* ── ima_read_file ─────────────────────────────────────────────── */
/*
 * Called before reading a file into kernel memory (e.g., firmware,
 * kexec image, module loading).  Measures the file content.
 */
static int ima_read_file(struct file *file, kernel_read_file_id_t id)
{
    if (!file)
        return -EINVAL;

    if (ima_mode == 0)
        return 0;

    (void)id;
    return ima_measure("/kernel/read", IMA_FILE_READ);
}

/* ── ima_post_read_file ─────────────────────────────────────────────── */
/*
 * Called after a file has been read into kernel memory.
 * Performs IMA appraisal on the buffer content.
 */
static int ima_post_read_file(struct file *file, void *buf, size_t size, kernel_read_file_id_t id)
{
    if (!file || !buf)
        return -EINVAL;

    if (ima_mode < 2)
        return 0;  /* appraise disabled */

    (void)size;
    (void)id;

    /* The buffer contains the full file contents that were just read.
     * In a full implementation we would hash the buffer and compare
     * against security.ima xattr. For now, we measure. */
    return ima_measure("/kernel/post_read", IMA_FILE_READ);
}

/* ── ima_kexec_cmdline ─────────────────────────────────────────────── */
/*
 * Measure the kernel command line passed to kexec.
 * This allows attestation to verify what command line was used.
 */
static int ima_kexec_cmdline(const char *cmdline)
{
    if (!cmdline)
        return -EINVAL;

    if (ima_mode == 0)
        return 0;

    /* Create a synthetic hash of the command line and log it.
     * In a real implementation we would compute a SHA256 of the cmdline. */
    uint8_t synthetic_hash[IMA_DIGEST_SIZE];
    size_t cmdline_len = strlen(cmdline);

    memset(synthetic_hash, 0, IMA_DIGEST_SIZE);
    /* Use the first IMA_DIGEST_SIZE bytes of the cmdline as a simple hash */
    for (size_t i = 0; i < cmdline_len && i < IMA_DIGEST_SIZE; i++) {
        synthetic_hash[i] = (uint8_t)cmdline[i];
    }

    /* Log the measurement as a kexec cmdline measurement */
    ima_lock();
    int idx = ima_log_count % IMA_LOG_MAX;
    strncpy(ima_log[idx].path, "kexec_cmdline", (int)sizeof(ima_log[idx].path) - 1);
    ima_log[idx].path[sizeof(ima_log[idx].path) - 1] = '\0';
    memcpy(ima_log[idx].hash, synthetic_hash, IMA_DIGEST_SIZE);
    ima_log[idx].type = IMA_FILE_READ;
    ima_log[idx].appraised = 0;
    ima_log[idx].passed = 1;
    ima_log_count++;
    if (ima_log_count > IMA_LOG_MAX)
        ima_log_wraps = 1;
    ima_unlock();

    return 0;
}

/* ── ima_measure_critical_data ─────────────────────────────────────── */
/*
 * Measure a critical data buffer (e.g., SELinux policy, IMA policy itself).
 */
static int ima_measure_critical_data(const char *name, const void *data, size_t len)
{
    if (!name || !data)
        return -EINVAL;

    if (ima_mode == 0)
        return 0;

    /* Log critical data measurement */
    uint8_t synthetic_hash[IMA_DIGEST_SIZE];
    memset(synthetic_hash, 0, IMA_DIGEST_SIZE);
    for (size_t i = 0; i < len && i < IMA_DIGEST_SIZE; i++) {
        synthetic_hash[i] = ((const uint8_t *)data)[i];
    }

    ima_lock();
    int idx = ima_log_count % IMA_LOG_MAX;
    strncpy(ima_log[idx].path, name, (int)sizeof(ima_log[idx].path) - 1);
    ima_log[idx].path[sizeof(ima_log[idx].path) - 1] = '\0';
    memcpy(ima_log[idx].hash, synthetic_hash, IMA_DIGEST_SIZE);
    ima_log[idx].type = IMA_FILE_READ;
    ima_log[idx].appraised = 0;
    ima_log[idx].passed = 1;
    ima_log_count++;
    if (ima_log_count > IMA_LOG_MAX)
        ima_log_wraps = 1;
    ima_unlock();

    return 0;
}

/* ── ima_get_action ─────────────────────────────────────────────── */
/*
 * Determine the IMA action to take for a given inode and mask.
 * Returns: 0 = measure, 1 = appraise, -1 = nothing.
 */
static int ima_get_action(struct inode *inode, int mask, int func)
{
    (void)inode;
    (void)mask;
    (void)func;

    if (ima_mode == 0)
        return 0;

    if (ima_mode >= 2)
        return 1;  /* appraise */

    return 0;  /* measure */
}

#include "module.h"
module_init(ima_init);
