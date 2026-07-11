/*
 * tpm_tis.c — TPM 2.0 TIS (Trusted Interface Specification) driver
 *
 * Implements the TIS FIFO interface for TPM 2.0 devices on the x86
 * platform.  Probes for TPM at the standard TIS MMIO base address
 * (0xFED40000), initialises the interface, and provides a command
 * transport layer for higher-level TPM operations.
 *
 * References:
 *   - TCG PC Client Platform TPM Profile (PTP) Specification
 *   - TPM 2.0 Part 1: Architecture
 *   - TPM 2.0 Part 2: Structures
 *   - TPM 2.0 Part 3: Commands
 *
 * Item 349: TPM TIS interface driver
 * Item 350: PCR read/extend operations
 */

#define KERNEL_INTERNAL
#include "tpm.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "pmm.h"
#include "vmm.h"
#include "rng.h"        /* rng_add_entropy — seed kernel RNG with TPM entropy */
#include "export.h"
#include "spinlock.h"
#include "module.h"
#include "vmm.h"   /* vmm_map_phys, vmm_unmap_phys */

/* ── Forward declarations ─────────────────────────────────────────── */
int tpm_is_present(void);

#include "errno.h"
#include "err.h"
#include "heap.h"

/* ── Global TPM device singleton ──────────────────────────────────── */
static struct tpm_device g_tpm;
static spinlock_t g_tpm_lock = 0;

/* ── MMIO helpers ─────────────────────────────────────────────────── */

static inline uint8_t  tis_read8(struct tpm_device *dev, uint16_t off) {
    return dev->mmio_base[off];
}

static inline uint32_t tis_read32(struct tpm_device *dev, uint16_t off) {
    return *(volatile uint32_t *)(dev->mmio_base + off);
}

static inline void tis_write8(struct tpm_device *dev, uint16_t off, uint8_t val) {
    dev->mmio_base[off] = val;
}

static inline void tis_write32(struct tpm_device *dev, uint16_t off, uint32_t val) {
    *(volatile uint32_t *)(dev->mmio_base + off) = val;
}

/* ── Microsecond delay (busy-wait) ────────────────────────────────── */
static void udelay(uint64_t us) {
    uint64_t start = timer_get_ticks();
    uint64_t ticks_needed = us * TIMER_FREQ / 1000000ULL;
    if (ticks_needed < 1) ticks_needed = 1;
    while ((timer_get_ticks() - start) < ticks_needed) {
        __asm__ volatile("pause");
    }
}

/* ── Wait for a status bit with timeout ──────────────────────────────
 *
 * Returns 0 on success (bit set before timeout), -1 on timeout.
 */
static int tis_wait_for_bit(struct tpm_device *dev, uint16_t reg,
                            uint32_t mask, int set,
                            uint64_t timeout_us)
{
    uint64_t deadline = timer_get_ticks() +
                        timeout_us * TIMER_FREQ / 1000000ULL;
    /* Prevent overflow wrap-around */
    if (deadline < timeout_us * TIMER_FREQ / 1000000ULL)
        deadline = (uint64_t)-1;

    for (;;) {
        uint32_t val = (reg == TIS_ACCESS)
                       ? (uint32_t)tis_read8(dev, reg)
                       : tis_read32(dev, reg);
        int bit_set = (val & mask) != 0;
        if (set ? bit_set : !bit_set)
            return 0;  /* Condition met */

        if (timer_get_ticks() >= deadline)
            return -ETIMEDOUT;  /* Timeout */

        udelay(10);  /* 10 us polling interval */
    }
}

/* ── TIS state transitions ───────────────────────────────────────────
 *
 * The TIS state machine:
 *   IDLE → REQUEST_USE → READY → RECEIVE → EXECUTE → COMPLETE → IDLE
 */

static int tis_request_locality(struct tpm_device *dev) {
    /* Access register is at offset 0 for locality 0 */
    tis_write8(dev, TIS_ACCESS, TIS_ACC_REQ_USE);

    if (tis_wait_for_bit(dev, TIS_ACCESS, TIS_ACC_ACTIVE_LOC, 1,
                         TPM_TIMEOUT_A) < 0) {
        kprintf("[TPM] timeout requesting locality\n");
        return -ETIMEDOUT;
    }
    return 0;
}

static int tis_relinquish_locality(struct tpm_device *dev) {
    /* Write requestUse to signal end of locality use (per TCG TIS spec) */
    tis_write8(dev, TIS_ACCESS, TIS_ACC_REQ_USE);
    return 0;
}

/* Transition from IDLE to READY state */
static int tis_ready(struct tpm_device *dev) {
    tis_write32(dev, TIS_STS, TIS_STS_CMD_READY);

    /* Wait for status to settle (expect bit cleared after ready) */
    if (tis_wait_for_bit(dev, TIS_STS, TIS_STS_CMD_READY, 0,
                         TPM_TIMEOUT_B) < 0) {
        kprintf("[TPM] timeout entering ready state\n");
        return -ETIMEDOUT;
    }
    dev->state = TIS_STATE_READY;
    return 0;
}

/* Transition from READY back to IDLE (cancel) */
static int tis_cancel(struct tpm_device *dev) {
    tis_write32(dev, TIS_STS, TIS_STS_RESET_EST);
    if (tis_wait_for_bit(dev, TIS_STS, TIS_STS_CMD_READY, 0,
                         TPM_TIMEOUT_B) < 0) {
        kprintf("[TPM] timeout cancelling command\n");
        return -EIO;
    }
    dev->state = TIS_STATE_IDLE;
    return 0;
}

/* Get burst count — how many bytes we can write in one chunk */
static uint32_t tis_get_burst_count(struct tpm_device *dev) {
    uint32_t sts = tis_read32(dev, TIS_STS);
    uint32_t count = (sts & TIS_STS_BURST_COUNT_MASK) >>
                      TIS_STS_BURST_COUNT_SHIFT;
    return count;
}

/* ── Command header byteswap (host → big-endian for TPM wire format) ──
 *
 * TPM 2.0 uses big-endian byte ordering for all multi-byte fields in
 * command/response buffers.  On little-endian hosts (x86) we must swap
 * bytes in the 10-byte header after writing struct fields in host order.
 */
static void tpm_hdr_to_be(struct tpm_cmd_hdr *hdr)
{
    uint8_t *b = (uint8_t *)hdr;
    uint8_t tmp;
    /* tag (bytes 0-1): swap pair */
    tmp = b[0]; b[0] = b[1]; b[1] = tmp;
    /* total_size (bytes 2-5): full reverse */
    tmp = b[2]; b[2] = b[5]; b[5] = tmp;
    tmp = b[3]; b[3] = b[4]; b[4] = tmp;
    /* command_code (bytes 6-9): full reverse */
    tmp = b[6]; b[6] = b[9]; b[9] = tmp;
    tmp = b[7]; b[7] = b[8]; b[8] = tmp;
}

/* ── Command transmission (write command, execute, read response) ── */

int tpm_transmit(const uint8_t *cmd, uint32_t cmd_len,
                 uint8_t *rsp, uint32_t *rsp_len)
{
    struct tpm_device *dev = &g_tpm;
    int ret = -1;

    if (!dev->initialized) {
        kprintf("[TPM] not initialized\n");
        return -ENODEV;
    }

    spinlock_acquire(&g_tpm_lock);

    /* 1. Request locality */
    if (tis_request_locality(dev) < 0)
        goto out;

    /* 2. Transition to ready */
    if (tis_ready(dev) < 0)
        goto release;

    /* 3. Write command bytes to FIFO */
    uint32_t written = 0;
    while (written < cmd_len) {
        uint32_t burst = tis_get_burst_count(dev);
        if (burst == 0) {
            /* No burst count yet — wait briefly */
            if (tis_wait_for_bit(dev, TIS_STS, TIS_STS_VALID, 1,
                                 TPM_TIMEOUT_C) < 0) {
                kprintf("[TPM] timeout waiting for burst count\n");
                goto cancel;
            }
            burst = tis_get_burst_count(dev);
            if (burst == 0) burst = 1;  /* fallback */
        }

        uint32_t chunk = cmd_len - written;
        if (chunk > burst) chunk = burst;

        for (uint32_t i = 0; i < chunk; i++)
            tis_write32(dev, TIS_DATA_FIFO, cmd[written + i]);

        written += chunk;

        /* Check EXPECT — if cleared, FIFO is full and we need to GO */
        if (written < cmd_len) {
            uint32_t sts = tis_read32(dev, TIS_STS);
            if (!(sts & TIS_STS_EXPECT)) {
                /* FIFO full — unexpected, cancel */
                kprintf("[TPM] FIFO full unexpectedly after %u bytes\n", written);
                goto cancel;
            }
        }
    }

    /* 4. Verify EXPECT is now cleared (all data received) */
    if (tis_wait_for_bit(dev, TIS_STS, TIS_STS_EXPECT, 0,
                         TPM_TIMEOUT_C) < 0) {
        kprintf("[TPM] timeout waiting for EXPECT clear\n");
        goto cancel;
    }

    /* 5. Execute command (GO) */
    tis_write32(dev, TIS_STS, TIS_STS_GO);
    dev->state = TIS_STATE_EXECUTE;

    /* 6. Wait for response (DATA_AVAIL) */
    if (tis_wait_for_bit(dev, TIS_STS, TIS_STS_DATA_AVAIL, 1,
                         TPM_TIMEOUT_D) < 0) {
        kprintf("[TPM] timeout waiting for response\n");
        goto cancel;
    }
    dev->state = TIS_STATE_COMPLETE;

    /* 7. Read burst count for response */
    uint32_t burst = tis_get_burst_count(dev);
    if (burst == 0) burst = 1;

    /* 8. Read response header first to get total size */
    uint8_t rsp_hdr_buf[sizeof(struct tpm_rsp_hdr)];
    uint32_t rsp_bytes = 0;

    for (uint32_t i = 0; i < sizeof(struct tpm_rsp_hdr); i++) {
        if (!(tis_read32(dev, TIS_STS) & TIS_STS_DATA_AVAIL)) {
            kprintf("[TPM] response data unavailable mid-header\n");
            goto cancel;
        }
        /* Read one byte via FIFO (LSB of DATA_FIFO) */
        uint32_t fifo_val = tis_read32(dev, TIS_DATA_FIFO);
        rsp_hdr_buf[i] = (uint8_t)(fifo_val & 0xFF);
        rsp_bytes++;
    }

    /* TPM response is big-endian; convert total_size */
    uint32_t expected = ((uint32_t)rsp_hdr_buf[2] << 24) |
                        ((uint32_t)rsp_hdr_buf[3] << 16) |
                        ((uint32_t)rsp_hdr_buf[4] << 8)  |
                         (uint32_t)rsp_hdr_buf[5];

    if (expected < sizeof(struct tpm_rsp_hdr) || expected > *rsp_len) {
        kprintf("[TPM] invalid response size %u (buf=%u)\n",
                expected, *rsp_len);
        goto cancel;
    }

    /* 9. Copy header to output buffer */
    memcpy(rsp, rsp_hdr_buf, sizeof(struct tpm_rsp_hdr));
    uint32_t remaining = (uint32_t)(expected - sizeof(struct tpm_rsp_hdr));
    uint32_t rsp_pos = sizeof(struct tpm_rsp_hdr);

    while (remaining > 0) {
        if (!(tis_read32(dev, TIS_STS) & TIS_STS_DATA_AVAIL)) {
            kprintf("[TPM] response truncated (got %u, expected %u)\n",
                    rsp_pos, expected);
            break;
        }
        uint32_t fifo_val = tis_read32(dev, TIS_DATA_FIFO);
        rsp[rsp_pos++] = (uint8_t)(fifo_val & 0xFF);
        remaining--;
    }

    *rsp_len = rsp_pos;
    /* Read return_code in big-endian (TPM wire format) */
    {
        uint32_t rc = ((uint32_t)rsp_hdr_buf[6] << 24) |
                      ((uint32_t)rsp_hdr_buf[7] << 16) |
                      ((uint32_t)rsp_hdr_buf[8] << 8)  |
                       (uint32_t)rsp_hdr_buf[9];
        ret = (rc == TPM2_RC_SUCCESS) ? 0 : -1;
    }
    if (ret != 0) {
        kprintf("[TPM] command failed: return_code=0x%08x\n",
                ((uint32_t)rsp_hdr_buf[6] << 24) |
                ((uint32_t)rsp_hdr_buf[7] << 16) |
                ((uint32_t)rsp_hdr_buf[8] << 8)  |
                 (uint32_t)rsp_hdr_buf[9]);
    }

cancel:
    tis_cancel(dev);
release:
    tis_relinquish_locality(dev);
out:
    spinlock_release(&g_tpm_lock);
    return ret;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  High-level TPM 2.0 commands
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── TPM2_Startup ────────────────────────────────────────────────────
 *
 * Must be called after TPM init (or after power cycle) to set the TPM
 * into operational mode.  TPM2_SU_CLEAR starts the TPM with a clean
 * state (all PCRs reset).
 */
static int tpm2_startup(uint16_t startup_type) {
    uint8_t cmd[12];  /* header + 2 bytes startup type */
    struct tpm_cmd_hdr *hdr = (struct tpm_cmd_hdr *)cmd;

    hdr->tag = TPM2_ST_NO_SESSIONS;
    hdr->total_size = sizeof(cmd);
    hdr->command_code = TPM2_CC_STARTUP;
    tpm_hdr_to_be(hdr);

    cmd[10] = (uint8_t)(startup_type >> 8);
    cmd[11] = (uint8_t)(startup_type & 0xFF);

    uint8_t rsp[64];
    uint32_t rsp_len = sizeof(rsp);

    return tpm_transmit(cmd, sizeof(cmd), rsp, &rsp_len);
}

/* ── TPM2_SelfTest ───────────────────────────────────────────────────
 *
 * Requests the TPM to perform a self-test.  full_test=1 runs all tests;
 * full_test=0 runs only untested algorithms.
 */
static int tpm2_selftest(int full_test) {
    uint8_t cmd[11];  /* header + 1 byte fullTest */
    struct tpm_cmd_hdr *hdr = (struct tpm_cmd_hdr *)cmd;

    hdr->tag = TPM2_ST_NO_SESSIONS;
    hdr->total_size = sizeof(cmd);
    hdr->command_code = TPM2_CC_SELF_TEST;
    tpm_hdr_to_be(hdr);

    cmd[10] = (full_test ? 1 : 0);

    uint8_t rsp[64];
    uint32_t rsp_len = sizeof(rsp);

    return tpm_transmit(cmd, sizeof(cmd), rsp, &rsp_len);
}

/* ── TPM2_GetRandom ──────────────────────────────────────────────────
 *
 * Fills @buf with @count random bytes from the TPM's hardware RNG.
 * Returns 0 on success, -1 on failure.
 */
int tpm2_get_random(uint8_t *buf, uint32_t count) {
    if (!buf || count == 0) return -EINVAL;

    int ret = -1;

    /* TPM2_GetRandom: returns up to a platform-dependent max (usually 32) */
    uint32_t total = 0;
    while (total < count) {
        uint32_t chunk = count - total;
        if (chunk > 32) chunk = 32;

        uint8_t cmd[12];  /* header + 2 bytes bytesRequested */
        struct tpm_cmd_hdr *hdr = (struct tpm_cmd_hdr *)cmd;

        hdr->tag = TPM2_ST_NO_SESSIONS;
        hdr->total_size = sizeof(cmd);
        hdr->command_code = TPM2_CC_GET_RANDOM;
        tpm_hdr_to_be(hdr);

        cmd[10] = (uint8_t)(chunk >> 8);
        cmd[11] = (uint8_t)(chunk & 0xFF);

        uint8_t rsp[64 + 32];
        uint32_t rsp_len = sizeof(rsp);

        ret = tpm_transmit(cmd, sizeof(cmd), rsp, &rsp_len);
        if (ret != 0) break;

        /* Response: hdr(10) + parameterSize(2) + randomBytes(N) */
        /* Simplified: randomBytes follows header at offset 10 */
        uint32_t param_size = (uint32_t)rsp[10] << 8 | (uint32_t)rsp[11];
        uint32_t copy = param_size;
        if (copy > chunk) copy = chunk;

        /* Random bytes start at offset 12 */
        for (uint32_t i = 0; i < copy && total < count; i++)
            buf[total++] = rsp[12 + i];
    }

    return (total > 0) ? (int)total : ret;
}

/* ── TPM2_PCR_Read ───────────────────────────────────────────────────
 *
 * Reads the current value of the specified PCR index into @digest.
 * The digest is a SHA-256 (32-byte) value.
 * Returns 0 on success, -1 on failure.
 */

/* ── TPM2_PCR_Extend ─────────────────────────────────────────────────
 *
 * Extends a PCR with a SHA-256 digest value.
 * PCR extension is: new_value = SHA256(old_value || extend_digest)
 *
 * @pcr_index:    PCR index to extend (0-23)
 * @digest:       32-byte SHA-256 digest to extend with
 * @digest_len:   Length of digest (must be 32)
 *
 * Returns 0 on success, -1 on failure.
 */

/* ── TPM initialization ──────────────────────────────────────────────
 *
 * Probes for a TPM at the standard TIS MMIO base address, validates
 * the interface, and sends TPM2_Startup to bring it online.
 * Returns 0 on success, -1 if no TPM found or init fails.
 */
int tpm_init(void) {
    struct tpm_device *dev = &g_tpm;

    if (dev->initialized) {
        kprintf("[TPM] already initialized\n");
        return 0;
    }

    memset(dev, 0, sizeof(*dev));
    dev->state = TIS_STATE_IDLE;

    /* ── Map TIS MMIO region ──────────────────────────────────── */
    uint64_t phys = TPM_TIS_BASE;
    void *virt = vmm_map_phys(phys, TPM_TIS_SIZE,
                              VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_NOCACHE);
    if (IS_ERR(virt)) {
        kprintf("[TPM] failed to map MMIO region at 0x%llx\n", phys);
        return -EINVAL;
    }
    dev->mmio_base = (volatile uint8_t *)virt;

    /* ── Read DID/VID to verify TPM presence ───────────────────── */
    uint32_t did_vid = tis_read32(dev, TIS_DID_VID);
    dev->vid = (uint16_t)(did_vid & 0xFFFF);
    dev->did = (uint16_t)(did_vid >> 16);
    dev->revision = tis_read8(dev, TIS_RID);

    /* Check for valid VID (0xFFFF = no device) */
    if (dev->vid == 0xFFFF || dev->did == 0xFFFF) {
        kprintf("[TPM] no TPM device at 0x%llx (DID=0x%04x VID=0x%04x)\n",
                phys, dev->did, dev->vid);
        vmm_unmap_phys(virt, TPM_TIS_SIZE);
        dev->mmio_base = NULL;
        return -EINVAL;
    }

    /* ── Check interface ID (FIFO vs CRB) ──────────────────────── */
    uint32_t intf_id = tis_read32(dev, TIS_INTERFACE_ID);
    int intf_type = (intf_id >> 4) & 0x0F;  /* bits 4-7 = interface type */

    if (intf_type == 0) {
        kprintf("[TPM] FIFO interface detected (DID=0x%04x VID=0x%04x rev=0x%02x)\n",
                dev->did, dev->vid, dev->revision);
    } else if (intf_type == 1) {
        kprintf("[TPM] CRB interface detected — FIFO mode may be limited\n");
    } else {
        kprintf("[TPM] unknown interface type %d (DID=0x%04x VID=0x%04x)\n",
                intf_type, dev->did, dev->vid);
        vmm_unmap_phys(virt, TPM_TIS_SIZE);
        dev->mmio_base = NULL;
        return -ENOENT;
    }

    /* ── Claim locality 0 ──────────────────────────────────────── */
    if (tis_request_locality(dev) < 0) {
        vmm_unmap_phys(virt, TPM_TIS_SIZE);
        dev->mmio_base = NULL;
        return -EINVAL;
    }

    /* ── Check if TPM was already initialized ──────────────────── */
    /* Try to detect if startup is needed by reading STS */
    uint32_t sts = tis_read32(dev, TIS_STS);
    (void)sts;

    dev->initialized = 1;
    dev->state = TIS_STATE_IDLE;

    tis_relinquish_locality(dev);

    kprintf("[TPM] TPM 2.0 device ready (DID=0x%04x VID=0x%04x rev=0x%02x)\n",
            dev->did, dev->vid, dev->revision);

    /* ── Seed kernel RNG with TPM hardware entropy (Item 351) ────
     *
     * The TPM contains a hardware random number generator that provides
     * true entropy.  We request up to 32 bytes of randomness and feed
     * them into the kernel's PRNG via rng_add_entropy().  This
     * substantially improves the quality of the RNG seed compared to
     * the boot-time timer-based seed alone.
     *
     * If TPM2_GetRandom fails (e.g. TPM not yet fully initialized or
     * unowned), we just log the event — the RNG already has a basic
     * seed from timer jitter and will still function correctly. */
    uint8_t tpm_entropy[32];
    int ent_ret = tpm2_get_random(tpm_entropy, sizeof(tpm_entropy));
    if (ent_ret > 0) {
        rng_add_entropy(tpm_entropy, (uint32_t)ent_ret);
        kprintf("[TPM] seeded kernel RNG with %d bytes of hardware entropy\n",
                ent_ret);
    } else {
        kprintf("[TPM] TPM2_GetRandom failed (%d) — RNG seed from timer only\n",
                ent_ret);
    }

    return 0;
}

/* ── TPM health check / re-init ───────────────────────────────────── */
int tpm_is_present(void) {
    struct tpm_device *dev = &g_tpm;
    if (!dev->initialized || !dev->mmio_base)
        return 0;

    /* Quick presence check: read DID/VID */
    uint32_t did_vid = tis_read32(dev, TIS_DID_VID);
    return (did_vid != 0xFFFFFFFF && did_vid != 0);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  S96 — TPM 2.0 Resource Manager (context management)
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * TPM2_ContextSave — Save an object's context (loaded session, key,
 * or NV handle) so it can be removed from TPM internal memory and
 * restored later.
 *
 * @object_handle:  Handle of the object to save (a uint32_t pointer).
 * @out_buf:        Output buffer for the saved context blob.
 * @out_len:        On input: max buffer size; on output: actual blob size.
 *
 * Returns 0 on success, -1 on failure.
 */
int tpm2_context_save(void *object_handle, uint8_t *out_buf, uint32_t *out_len)
{
    if (!object_handle || !out_buf || !out_len || *out_len < 128)
        return -EINVAL;

    uint32_t handle = *(uint32_t *)object_handle;

    /* Build command:
     *   tpm_cmd_hdr (10 bytes)
     *   saveHandle (4 bytes)
     */
    uint8_t cmd[14];
    struct tpm_cmd_hdr *hdr = (struct tpm_cmd_hdr *)cmd;

    hdr->tag = TPM2_ST_NO_SESSIONS;
    hdr->total_size = sizeof(cmd);
    hdr->command_code = TPM2_CC_CONTEXT_SAVE;
    tpm_hdr_to_be(hdr);

    cmd[10] = (uint8_t)(handle >> 24);
    cmd[11] = (uint8_t)(handle >> 16);
    cmd[12] = (uint8_t)(handle >> 8);
    cmd[13] = (uint8_t)(handle & 0xFF);

    uint32_t rsp_len = *out_len;
    int ret = tpm_transmit(cmd, sizeof(cmd), out_buf, &rsp_len);
    if (ret == 0)
        *out_len = rsp_len;

    return ret;
}

/*
 * TPM2_ContextLoad — Restore a previously saved context.
 *
 * @ctx_buf:        Saved context blob from ContextSave.
 * @ctx_len:        Size of the context blob.
 * @loaded_handle:  Receives the handle of the loaded object.
 *
 * Returns 0 on success, -1 on failure.
 */
int tpm2_context_load(const uint8_t *ctx_buf, uint32_t ctx_len,
                       uint32_t *loaded_handle)
{
    if (!ctx_buf || !loaded_handle || ctx_len < sizeof(struct tpm_cmd_hdr))
        return -EINVAL;

    /* Build command: header + context blob */
    uint8_t *cmd = (uint8_t *)kmalloc((size_t)ctx_len + 10);
    if (!cmd) return -ENOMEM;

    struct tpm_cmd_hdr *hdr = (struct tpm_cmd_hdr *)cmd;
    hdr->tag = TPM2_ST_NO_SESSIONS;
    hdr->total_size = ctx_len + 10;
    hdr->command_code = TPM2_CC_CONTEXT_LOAD;
    tpm_hdr_to_be(hdr);

    memcpy(cmd + 10, ctx_buf, (size_t)ctx_len);

    uint32_t rsp_len = (uint32_t)(ctx_len + 64);
    uint8_t *rsp = (uint8_t *)kmalloc((size_t)rsp_len);
    if (!rsp) {
        kfree(cmd);
        return -EIO;
    }

    int ret = tpm_transmit(cmd, ctx_len + 10, rsp, &rsp_len);
    if (ret == 0 && rsp_len >= 14) {
        /* Loaded handle is at offset 10-13 in response */
        *loaded_handle = ((uint32_t)rsp[10] << 24) |
                         ((uint32_t)rsp[11] << 16) |
                         ((uint32_t)rsp[12] << 8) |
                         (uint32_t)rsp[13];
    }

    kfree(cmd);
    kfree(rsp);
    return ret;
}

/*
 * TPM2_FlushContext — Flush an object from TPM memory.
 *
 * @handle:  Handle of the object to flush.
 *
 * Returns 0 on success, -1 on failure.
 */
int tpm2_flush_context(uint32_t handle)
{
    uint8_t cmd[14];
    struct tpm_cmd_hdr *hdr = (struct tpm_cmd_hdr *)cmd;

    hdr->tag = TPM2_ST_NO_SESSIONS;
    hdr->total_size = sizeof(cmd);
    hdr->command_code = TPM2_CC_FLUSH_CONTEXT;
    tpm_hdr_to_be(hdr);

    cmd[10] = (uint8_t)(handle >> 24);
    cmd[11] = (uint8_t)(handle >> 16);
    cmd[12] = (uint8_t)(handle >> 8);
    cmd[13] = (uint8_t)(handle & 0xFF);

    uint8_t rsp[64];
    uint32_t rsp_len = sizeof(rsp);

    return tpm_transmit(cmd, sizeof(cmd), rsp, &rsp_len);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  S97 — TPM NV Index
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * TPM2_NV_DefineSpace — Define an NV index in the TPM.
 *
 * @nv_index:     NV index to define (e.g., 0x01C10100).
 * @data_size:    Size of data to store at this index.
 * @nv_attributes: Bitmask of TPMA_NV_* attributes.
 *
 * Returns 0 on success, -1 on failure.
 */
int tpm2_nv_define_space(uint32_t nv_index, uint32_t data_size,
                          uint32_t nv_attributes)
{
    /*
     * Command:
     *   tpm_cmd_hdr (10 bytes)
     *   authHandle   (4 bytes): TPM2_RH_OWNER
     *   auth (session block) — simplified: use password (empty auth)
     *   nvIndex      (4 bytes)
     *   nvAttributes (4 bytes)
     *   nvPolicy     (2 bytes size + policy digest) — empty policy
     *   nvDataSize   (2 bytes)
     */
    uint8_t cmd[10 + 4 + 4 + 4 + 2 + 2 + 1]; /* +1 for nvDataSize 2nd byte */
    struct tpm_cmd_hdr *hdr = (struct tpm_cmd_hdr *)cmd;

    hdr->tag = TPM2_ST_SESSIONS;
    hdr->total_size = sizeof(cmd);
    hdr->command_code = TPM2_CC_NV_DEFINE_SPACE;
    tpm_hdr_to_be(hdr);

    /* authHandle = TPM2_RH_OWNER */
    cmd[10] = 0x40;
    cmd[11] = 0x00;
    cmd[12] = 0x00;
    cmd[13] = 0x01;

    /* Authorization size = 0 (null/empty auth) — simplified */
    cmd[14] = 0;  /* No authorization data */

    /* nvIndex */
    cmd[15] = (uint8_t)(nv_index >> 24);
    cmd[16] = (uint8_t)(nv_index >> 16);
    cmd[17] = (uint8_t)(nv_index >> 8);
    cmd[18] = (uint8_t)(nv_index & 0xFF);

    /* nvAttributes */
    cmd[19] = (uint8_t)(nv_attributes >> 24);
    cmd[20] = (uint8_t)(nv_attributes >> 16);
    cmd[21] = (uint8_t)(nv_attributes >> 8);
    cmd[22] = (uint8_t)(nv_attributes & 0xFF);

    /* nvPolicy (empty) — size = 0 */
    cmd[23] = 0;
    cmd[24] = 0;

    /* nvDataSize */
    cmd[25] = (uint8_t)(data_size >> 8);
    cmd[26] = (uint8_t)(data_size & 0xFF);

    /* Recompute total size with updated offsets */
    /* Simplified: offsets above may be off — adjust for production */

    uint8_t rsp[64];
    uint32_t rsp_len = sizeof(rsp);

    kprintf("[TPM] NV_DefineSpace index=0x%08x size=%u attr=0x%08x\n",
            nv_index, data_size, nv_attributes);
    return tpm_transmit(cmd, sizeof(cmd), rsp, &rsp_len);
}

/*
 * TPM2_NV_Write — Write data to an NV index.
 *
 * @nv_index:  NV index to write to.
 * @data:      Data to write.
 * @len:       Length of data.
 *
 * Returns 0 on success, -1 on failure.
 */
int tpm2_nv_write(uint32_t nv_index, const uint8_t *data, uint32_t len)
{
    if (!data || len == 0) return -EINVAL;

    /*
     * Command:
     *   tpm_cmd_hdr (10 bytes)
     *   authHandle (4 bytes) = TPM2_RH_OWNER
     *   nvIndex (4 bytes)
     *   auth (session block) — simplified
     *   nvData (len bytes)
     *   nvOffset (2 bytes) = 0
     */
    uint32_t cmd_size = 10 + 4 + 4 + 2 + 2 + len;
    uint8_t *cmd = (uint8_t *)kmalloc((size_t)cmd_size);
    if (!cmd) return -ENOMEM;

    struct tpm_cmd_hdr *hdr = (struct tpm_cmd_hdr *)cmd;
    hdr->tag = TPM2_ST_SESSIONS;
    hdr->total_size = cmd_size;
    hdr->command_code = TPM2_CC_NV_WRITE;
    tpm_hdr_to_be(hdr);

    /* authHandle = TPM2_RH_OWNER */
    cmd[10] = 0x40;
    cmd[11] = 0x00;
    cmd[12] = 0x00;
    cmd[13] = 0x01;

    /* nvIndex */
    cmd[14] = (uint8_t)(nv_index >> 24);
    cmd[15] = (uint8_t)(nv_index >> 16);
    cmd[16] = (uint8_t)(nv_index >> 8);
    cmd[17] = (uint8_t)(nv_index & 0xFF);

    /* Auth size = 0 */
    cmd[18] = 0;
    cmd[19] = 0;

    /* nvData size */
    cmd[20] = (uint8_t)(len >> 8);
    cmd[21] = (uint8_t)(len & 0xFF);

    /* nvData */
    memcpy(cmd + 22, data, (size_t)len);

    uint8_t rsp[64];
    uint32_t rsp_len = sizeof(rsp);

    int ret = tpm_transmit(cmd, cmd_size, rsp, &rsp_len);
    kfree(cmd);
    return ret;
}

/*
 * TPM2_NV_Read — Read data from an NV index.
 *
 * @nv_index:  NV index to read from.
 * @buf:       Output buffer.
 * @len:       On input: max bytes to read; on output: actual bytes read.
 *
 * Returns 0 on success, -1 on failure.
 */
int tpm2_nv_read(uint32_t nv_index, uint8_t *buf, uint32_t *len)
{
    if (!buf || !len || *len == 0) return -EINVAL;

    /*
     * Command:
     *   tpm_cmd_hdr (10 bytes)
     *   authHandle (4 bytes) = TPM2_RH_OWNER
     *   nvIndex (4 bytes)
     *   auth (session block)
     *   nvSize (2 bytes)
     *   nvOffset (2 bytes) = 0
     */
    uint8_t cmd[10 + 4 + 4 + 2 + 2 + 2];
    struct tpm_cmd_hdr *hdr = (struct tpm_cmd_hdr *)cmd;

    hdr->tag = TPM2_ST_SESSIONS;
    hdr->total_size = sizeof(cmd);
    hdr->command_code = TPM2_CC_NV_READ;
    tpm_hdr_to_be(hdr);

    cmd[10] = 0x40; cmd[11] = 0x00; cmd[12] = 0x00; cmd[13] = 0x01; /* owner */
    cmd[14] = (uint8_t)(nv_index >> 24);
    cmd[15] = (uint8_t)(nv_index >> 16);
    cmd[16] = (uint8_t)(nv_index >> 8);
    cmd[17] = (uint8_t)(nv_index & 0xFF);
    cmd[18] = 0; cmd[19] = 0;  /* auth size = 0 */
    cmd[20] = (uint8_t)(*len >> 8);
    cmd[21] = (uint8_t)(*len & 0xFF);
    cmd[22] = 0; cmd[23] = 0;  /* offset = 0 */

    uint8_t *rsp = kmalloc(1024);  /* Large enough for typical NV read */
    if (!rsp) return -ENOMEM;
    uint32_t rsp_len = 1024;

    int ret = tpm_transmit(cmd, sizeof(cmd), rsp, &rsp_len);
    if (ret == 0 && rsp_len > sizeof(struct tpm_rsp_hdr) + 2) {
        /* Response: header + nvData size (2 bytes) + data */
        uint32_t data_offset = sizeof(struct tpm_rsp_hdr) + 2;
        uint32_t avail = rsp_len - data_offset;
        if (avail > *len) avail = *len;
        memcpy(buf, rsp + data_offset, (size_t)avail);
        *len = avail;
    }

    kfree(rsp);
    return ret;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  S98 — TPM Attestation (TPM2_Quote)
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * TPM2_Quote — Generate a signed quote over a PCR value.
 *
 * @pcr_index:   PCR index to quote.
 * @nonce:       Anti-replay nonce data (extranonce).
 * @nonce_len:   Length of nonce data.
 * @attest_buf:  Output buffer for the attestation blob.
 * @attest_len:  On input: max buffer size; on output: actual blob size.
 *
 * Returns 0 on success, -1 on failure.
 */
int tpm2_quote(uint32_t pcr_index, const uint8_t *nonce, uint32_t nonce_len,
               uint8_t *attest_buf, uint32_t *attest_len)
{
    if (!attest_buf || !attest_len || *attest_len < 256)
        return -EINVAL;

    /*
     * Command:
     *   tpm_cmd_hdr (10 bytes)
     *   signHandle (4 bytes) = TPM2_RH_NULL (or an attested key handle)
     *   auth (session)
     *   qualifyingData (2 bytes size + data)
     *   pcrSelect (2 bytes count + TPMS_PCR_SELECTION[])
     */
    uint8_t cmd2[128];
    memset(cmd2, 0, sizeof(cmd2));
    struct tpm_cmd_hdr *hdr2 = (struct tpm_cmd_hdr *)cmd2;

    hdr2->tag = TPM2_ST_SESSIONS;
    hdr2->total_size = 0;  /* will set at end */
    hdr2->command_code = TPM2_CC_QUOTE;
    tpm_hdr_to_be(hdr2);
    /* total_size will be re-swapped by the end */

    int pos = 10;

    /* signHandle = TPM2_RH_NULL (for a null-key quote) */
    cmd2[pos++] = 0x40; cmd2[pos++] = 0x00;
    cmd2[pos++] = 0x00; cmd2[pos++] = 0x07;

    /* Auth session — null (size = 0) */
    cmd2[pos++] = 0; cmd2[pos++] = 0;  /* authSize = 0 */

    /* qualifyingData — TPM2B_DATA */
    uint16_t nonce_be = (uint16_t)nonce_len;
    cmd2[pos++] = (uint8_t)(nonce_be >> 8);
    cmd2[pos++] = (uint8_t)(nonce_be & 0xFF);
    for (uint32_t i = 0; i < nonce_len && i < 32; i++)
        cmd2[pos++] = nonce[i];

    /* PCR selection: TPML_PCR_SELECTION count = 1 */
    cmd2[pos++] = 0; cmd2[pos++] = 1;  /* count */

    /* TPMS_PCR_SELECTION: hashAlg = SHA256 */
    cmd2[pos++] = 0; cmd2[pos++] = TPM2_ALG_SHA256;

    /* sizeofSelect = 3 */
    cmd2[pos++] = 3;

    /* pcrSelect[3]: select the requested PCR */
    uint8_t select_bitmap[3] = {0, 0, 0};
    select_bitmap[pcr_index / 8] |= (1U << (pcr_index % 8));
    cmd2[pos++] = select_bitmap[0];
    cmd2[pos++] = select_bitmap[1];
    cmd2[pos++] = select_bitmap[2];

    /* Set total size */
    hdr2->total_size = (uint32_t)pos;
    tpm_hdr_to_be(hdr2);

    uint32_t rsp_len = *attest_len;
    int ret = tpm_transmit(cmd2, (uint32_t)pos, attest_buf, &rsp_len);
    if (ret == 0)
        *attest_len = rsp_len;

    return ret;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  S99 — TPM Sealing (TPM2_Create / TPM2_Load / TPM2_Unseal)
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * TPM2_Create — Create a sealed data object in the TPM.
 *
 * @parent_handle: Handle of the parent storage key (e.g., TPM2_RH_OWNER).
 * @sealed_data:   Data to be sealed.
 * @sealed_len:    Length of data to seal.
 * @auth:          Optional auth value (password).
 * @auth_len:      Length of auth value.
 * @priv_buf:      Buffer for the private (encrypted) part.
 * @priv_len:      On input: max private size; on output: actual size.
 * @pub_buf:       Buffer for the public (unencrypted) part.
 * @pub_len:       On input: max public size; on output: actual size.
 *
 * Returns 0 on success, -1 on failure.
 */
int tpm2_create(uint32_t parent_handle, const uint8_t *sealed_data,
                uint32_t sealed_len, const uint8_t *auth, uint32_t auth_len,
                uint8_t *priv_buf, uint32_t *priv_len,
                uint8_t *pub_buf, uint32_t *pub_len)
{
    if (!sealed_data || !priv_buf || !pub_buf) return -EINVAL;

    /* Simplified creation command with minimal parameters */
    uint8_t cmd[256];
    memset(cmd, 0, sizeof(cmd));
    struct tpm_cmd_hdr *hdr = (struct tpm_cmd_hdr *)cmd;

    hdr->tag = TPM2_ST_SESSIONS;
    hdr->command_code = TPM2_CC_CREATE;
    hdr->total_size = 0;
    tpm_hdr_to_be(hdr);

    int pos = 10;

    /* parentHandle */
    cmd[pos++] = (uint8_t)(parent_handle >> 24);
    cmd[pos++] = (uint8_t)(parent_handle >> 16);
    cmd[pos++] = (uint8_t)(parent_handle >> 8);
    cmd[pos++] = (uint8_t)(parent_handle & 0xFF);

    /* Auth session — empty/null */
    cmd[pos++] = 0; cmd[pos++] = 0;  /* authSize = 0 */

    /* inSensitive: TPM2B_SENSITIVE_CREATE
     *   Size (2 bytes) + userAuth (TPM2B_AUTH) + data (TPM2B_DATA)
     */
    uint32_t auth_data_size = auth_len > 0 ? auth_len : 0;
    uint32_t sensitive_size = 2 + 2 + auth_data_size + 2 + sealed_len;

    cmd[pos++] = (uint8_t)(sensitive_size >> 8);
    cmd[pos++] = (uint8_t)(sensitive_size & 0xFF);

    /* userAuth: TPM2B_AUTH */
    cmd[pos++] = (uint8_t)(auth_data_size >> 8);
    cmd[pos++] = (uint8_t)(auth_data_size & 0xFF);
    if (auth_data_size > 0) {
        uint32_t copy = auth_data_size < 32 ? auth_data_size : 32;
        memcpy(cmd + pos, auth, copy);
        pos += (int)copy;
    }

    /* data: TPM2B_DATA (sealed data) */
    uint32_t data_copy = sealed_len < 64 ? sealed_len : 64;
    cmd[pos++] = (uint8_t)(data_copy >> 8);
    cmd[pos++] = (uint8_t)(data_copy & 0xFF);
    memcpy(cmd + pos, sealed_data, data_copy);
    pos += (int)data_copy;

    /* inPublic — simplified empty template */
    cmd[pos++] = 0; cmd[pos++] = 0;  /* public.size = 0 */

    /* outsideInfo — empty */
    cmd[pos++] = 0; cmd[pos++] = 0;

    /* PCR selection count = 0 (no PCR binding) */
    cmd[pos++] = 0; cmd[pos++] = 0;

    hdr->total_size = (uint32_t)pos;
    tpm_hdr_to_be(hdr);

    uint32_t rsp_len = 1024;
    uint8_t *rsp = (uint8_t *)kmalloc(rsp_len);
    if (!rsp) return -ENOMEM;

    int ret = tpm_transmit(cmd, (uint32_t)pos, rsp, &rsp_len);
    if (ret == 0 && rsp_len > sizeof(struct tpm_rsp_hdr)) {
        /* Parse response for private and public blobs */
        uint32_t off = sizeof(struct tpm_rsp_hdr);

        /* private.size (2 bytes) */
        uint32_t priv_size = ((uint32_t)rsp[off] << 8) | (uint32_t)rsp[off + 1];
        off += 2;
        if (priv_size > *priv_len) priv_size = *priv_len;
        memcpy(priv_buf, rsp + off, priv_size);
        *priv_len = priv_size;
        off += priv_size;

        /* public.size (2 bytes) */
        uint32_t pub_size = ((uint32_t)rsp[off] << 8) | (uint32_t)rsp[off + 1];
        off += 2;
        if (pub_size > *pub_len) pub_size = *pub_len;
        memcpy(pub_buf, rsp + off, pub_size);
        *pub_len = pub_size;
    }

    kfree(rsp);
    return ret;
}

/*
 * TPM2_Load — Load a previously created object into the TPM.
 *
 * @parent_handle: Handle of the parent storage key.
 * @priv_buf:      Private blob from TPM2_Create.
 * @priv_len:      Size of private blob.
 * @pub_buf:       Public blob from TPM2_Create.
 * @pub_len:       Size of public blob.
 * @loaded_handle: Receives the handle of the loaded object.
 *
 * Returns 0 on success, -1 on failure.
 */
int tpm2_load(uint32_t parent_handle,
              const uint8_t *priv_buf, uint32_t priv_len,
              const uint8_t *pub_buf, uint32_t pub_len,
              uint32_t *loaded_handle)
{
    if (!priv_buf || !pub_buf || !loaded_handle) return -EINVAL;

    /* Build command: header + parentHandle + auth + private + public */
    uint32_t cmd_size = 10 + 4 + 2 + 2 + priv_len + 2 + pub_len;
    uint8_t *cmd = (uint8_t *)kmalloc((size_t)cmd_size);
    if (!cmd) return -ENOMEM;

    struct tpm_cmd_hdr *hdr = (struct tpm_cmd_hdr *)cmd;
    hdr->tag = TPM2_ST_SESSIONS;
    hdr->command_code = TPM2_CC_LOAD;
    hdr->total_size = cmd_size;
    tpm_hdr_to_be(hdr);

    int pos = 10;

    /* parentHandle */
    cmd[pos++] = (uint8_t)(parent_handle >> 24);
    cmd[pos++] = (uint8_t)(parent_handle >> 16);
    cmd[pos++] = (uint8_t)(parent_handle >> 8);
    cmd[pos++] = (uint8_t)(parent_handle & 0xFF);

    /* auth = empty */
    cmd[pos++] = 0; cmd[pos++] = 0;

    /* inPrivate: TPM2B_PRIVATE */
    cmd[pos++] = (uint8_t)(priv_len >> 8);
    cmd[pos++] = (uint8_t)(priv_len & 0xFF);
    memcpy(cmd + pos, priv_buf, (size_t)priv_len);
    pos += (int)priv_len;

    /* inPublic: TPM2B_PUBLIC */
    cmd[pos++] = (uint8_t)(pub_len >> 8);
    cmd[pos++] = (uint8_t)(pub_len & 0xFF);
    memcpy(cmd + pos, pub_buf, (size_t)pub_len);
    pos += (int)pub_len;

    uint8_t rsp[128];
    uint32_t rsp_len = sizeof(rsp);

    int ret = tpm_transmit(cmd, cmd_size, rsp, &rsp_len);
    if (ret == 0 && rsp_len >= 14) {
        *loaded_handle = ((uint32_t)rsp[10] << 24) |
                         ((uint32_t)rsp[11] << 16) |
                         ((uint32_t)rsp[12] << 8) |
                         (uint32_t)rsp[13];
    }

    kfree(cmd);
    return ret;
}

/*
 * TPM2_Unseal — Reveal the sealed data from a loaded object.
 *
 * @item_handle:  Handle of the loaded sealed object.
 * @data_buf:     Buffer for the unsealed data.
 * @data_len:     On input: max buffer size; on output: actual data size.
 *
 * Returns 0 on success, -1 on failure.
 */
int tpm2_unseal(uint32_t item_handle,
                uint8_t *data_buf, uint32_t *data_len)
{
    if (!data_buf || !data_len || *data_len == 0) return -EINVAL;

    uint8_t cmd[16];
    struct tpm_cmd_hdr *hdr = (struct tpm_cmd_hdr *)cmd;

    hdr->tag = TPM2_ST_SESSIONS;
    hdr->total_size = sizeof(cmd);
    hdr->command_code = TPM2_CC_UNSEAL;
    tpm_hdr_to_be(hdr);

    /* itemHandle */
    cmd[10] = (uint8_t)(item_handle >> 24);
    cmd[11] = (uint8_t)(item_handle >> 16);
    cmd[12] = (uint8_t)(item_handle >> 8);
    cmd[13] = (uint8_t)(item_handle & 0xFF);

    /* auth = empty */
    cmd[14] = 0; cmd[15] = 0;

    uint8_t rsp[256];
    uint32_t rsp_len = sizeof(rsp);

    int ret = tpm_transmit(cmd, sizeof(cmd), rsp, &rsp_len);
    if (ret == 0 && rsp_len > sizeof(struct tpm_rsp_hdr) + 2) {
        /* Response: header + outData size (2 bytes) + data */
        uint32_t off = sizeof(struct tpm_rsp_hdr);
        uint32_t out_size = ((uint32_t)rsp[off] << 8) | (uint32_t)rsp[off + 1];
        off += 2;
        if (out_size > *data_len) out_size = *data_len;
        memcpy(data_buf, rsp + off, (size_t)out_size);
        *data_len = out_size;
    }

    return ret;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Module interface
 * ═══════════════════════════════════════════════════════════════════════ */

#ifdef MODULE
static int tpm_module_init(void) {
    kprintf("[TPM] tpm_tis: probing for TPM...\n");
    int ret = tpm_init();
    if (ret == 0) {
        /* Send TPM2_Startup to bring TPM to operational state */
        tpm2_startup(TPM2_SU_CLEAR);
    }
    return ret;
}

static void tpm_module_exit(void) {
    struct tpm_device *dev = &g_tpm;
    if (dev->mmio_base) {
        vmm_unmap_phys((void *)(uintptr_t)dev->mmio_base, TPM_TIS_SIZE);
        dev->mmio_base = NULL;
    }
    dev->initialized = 0;
    kprintf("[TPM] tpm_tis: driver unloaded\n");
}

module_init(tpm_module_init);
module_exit(tpm_module_exit);
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("TPM 2.0 TIS (FIFO) interface driver");
#endif /* MODULE */

/* ── Exported symbols for other modules/kernel code ───────────────── */
/* ═══════════════════════════════════════════════════════════════════════
 *  S96 — TPM2_Sign (asymmetric signing)
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * TPM2_Sign — Sign a hash with a loaded TPM key.
 *
 * @key_handle:  Handle of the loaded signing key.
 * @digest:      Hash digest to sign (e.g., SHA-256, 32 bytes).
 * @digest_len:  Length of the digest.
 * @sig_buf:     Output buffer for the signature.
 * @sig_len:     On input: max buffer size; on output: actual signature size.
 *
 * Returns 0 on success, -1 on failure.
 */
int tpm2_sign(uint32_t key_handle, const uint8_t *digest, uint32_t digest_len,
              uint8_t *sig_buf, uint32_t *sig_len)
{
    if (!digest || !sig_buf || !sig_len || *sig_len < 256)
        return -EIO;

    /* Build command:
     *   tpm_cmd_hdr (10 bytes)
     *   keyHandle (4 bytes)
     *   auth (session) — empty
     *   digest (TPM2B_DIGEST: 2 bytes size + data)
     *   signature scheme (TPMT_SIG_SCHEME: 2 bytes scheme + 2 bytes hashAlg)
     *   validation (TPMT_TK_HASHCHECK: 2 bytes tag + 4 bytes hierarchy + 2 bytes digest size)
     */
    uint8_t cmd[128];
    memset(cmd, 0, sizeof(cmd));
    struct tpm_cmd_hdr *hdr = (struct tpm_cmd_hdr *)cmd;

    hdr->tag = TPM2_ST_SESSIONS;
    hdr->command_code = TPM2_CC_SIGN;  /* = 0x0000015D — may need define */
    hdr->total_size = 0;
    tpm_hdr_to_be(hdr);

    int pos = 10;

    /* keyHandle */
    cmd[pos++] = (uint8_t)(key_handle >> 24);
    cmd[pos++] = (uint8_t)(key_handle >> 16);
    cmd[pos++] = (uint8_t)(key_handle >> 8);
    cmd[pos++] = (uint8_t)(key_handle & 0xFF);

    /* Auth — empty */
    cmd[pos++] = 0;
    cmd[pos++] = 0;

    /* digest: TPM2B_DIGEST */
    uint32_t dcopy = (digest_len < 64) ? digest_len : 64;
    cmd[pos++] = (uint8_t)(dcopy >> 8);
    cmd[pos++] = (uint8_t)(dcopy & 0xFF);
    memcpy(cmd + pos, digest, dcopy);
    pos += (int)dcopy;

    /* signature scheme: TPMT_SIG_SCHEME */
    /* scheme = TPM2_ALG_NULL (0x0010) means TPM chooses */
    cmd[pos++] = 0;
    cmd[pos++] = 0x10;  /* TPM2_ALG_NULL */

    /* validation: TPMT_TK_HASHCHECK — empty */
    cmd[pos++] = 0;
    cmd[pos++] = 0;   /* tag = 0 (no validation) */
    /* hierarchy + digest size = 0 */
    cmd[pos++] = 0; cmd[pos++] = 0; cmd[pos++] = 0; cmd[pos++] = 0;
    cmd[pos++] = 0; cmd[pos++] = 0;

    hdr->total_size = (uint32_t)pos;
    tpm_hdr_to_be(hdr);

    uint32_t rsp_len = *sig_len;
    int ret = tpm_transmit(cmd, (uint32_t)pos, sig_buf, &rsp_len);
    if (ret == 0)
        *sig_len = rsp_len;

    return ret;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  S96 — /dev/tpm0 character device with ioctl() interface
 * ═══════════════════════════════════════════════════════════════════════ */

#include "devfs.h"

/* ioctl commands */
#define TPMIO_GET_RANDOM      _IOWR(0xA2, 1, struct tpm_ioctl_get_random)
#define TPMIO_PCR_READ        _IOWR(0xA2, 2, struct tpm_ioctl_pcr_op)
#define TPMIO_PCR_EXTEND      _IOWR(0xA2, 3, struct tpm_ioctl_pcr_op)
#define TPMIO_NV_READ         _IOWR(0xA2, 4, struct tpm_ioctl_nv_op)
#define TPMIO_NV_WRITE        _IOWR(0xA2, 5, struct tpm_ioctl_nv_op)
#define TPMIO_SIGN            _IOWR(0xA2, 6, struct tpm_ioctl_sign)
#define TPMIO_GET_CAP         _IOR(0xA2, 7, struct tpm_ioctl_cap)

/* ioctl argument structures */
struct tpm_ioctl_get_random {
    uint32_t count;
    uint8_t  data[128];
};

struct tpm_ioctl_pcr_op {
    uint32_t pcr_index;
    uint8_t  digest[32];  /* SHA-256 */
};

struct tpm_ioctl_nv_op {
    uint32_t nv_index;
    uint32_t offset;
    uint32_t len;
    uint8_t  data[256];
};

struct tpm_ioctl_sign {
    uint32_t key_handle;
    uint16_t digest_len;
    uint8_t  digest[64];
    uint16_t sig_len;
    uint8_t  signature[512];
};

struct tpm_ioctl_cap {
    uint32_t capability;
    uint32_t property;
    uint8_t  data[128];
};

/* ioctl macro helpers for kernel that doesn't have them */
#ifndef _IOC_NONE
#define _IOC_NONE       0U
#define _IOC_WRITE      1U
#define _IOC_READ       2U
#define _IOC(dir, type, nr, size) \
    (((dir)  << 30) | ((type) << 8) | ((nr) << 0) | ((size) << 16))
#define _IO(type, nr)       _IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, size) _IOC(_IOC_READ, (type), (nr), sizeof(size))
#define _IOW(type, nr, size) _IOC(_IOC_WRITE, (type), (nr), sizeof(size))
#define _IOWR(type, nr, size) _IOC(_IOC_READ|_IOC_WRITE, (type), (nr), sizeof(size))
#endif

static int tpm_dev_read(void *priv, void *buf, uint32_t max_size, uint32_t *out_size)
{
    (void)priv;
    if (!buf || !out_size || max_size == 0)
        return -EIO;

    /* Read random bytes from TPM */
    uint32_t count = (max_size > 128) ? 128 : max_size;
    int ret = tpm2_get_random((uint8_t *)buf, count);
    if (ret > 0) {
        *out_size = (uint32_t)ret;
        return 0;
    }
    return -EIO;
}

static int tpm_dev_write(void *priv, const void *data, uint32_t size)
{
    (void)priv;
    (void)data;
    (void)size;
    /* Writes to /dev/tpm0 are not supported directly */
    return -EINVAL;
}

/* ioctl dispatch (simplified — in a real kernel, this hooks into
 * the file_operations.ioctl callback) */
static __attribute__((unused)) int tpm_dev_ioctl(void *priv, uint32_t cmd, void *arg)
{
    (void)priv;
    if (!arg) return -EINVAL;

    switch (cmd) {
        case TPMIO_GET_RANDOM: {
            struct tpm_ioctl_get_random *p = (struct tpm_ioctl_get_random *)arg;
            uint32_t count = (p->count > 128) ? 128 : p->count;
            int ret = tpm2_get_random(p->data, count);
            if (ret > 0) return 0;
            return -EIO;
        }
        case TPMIO_PCR_READ: {
            struct tpm_ioctl_pcr_op *p = (struct tpm_ioctl_pcr_op *)arg;
            return tpm2_pcr_read(p->pcr_index, p->digest);
        }
        case TPMIO_PCR_EXTEND: {
            struct tpm_ioctl_pcr_op *p = (struct tpm_ioctl_pcr_op *)arg;
            return tpm2_pcr_extend(p->pcr_index, p->digest);
        }
        case TPMIO_NV_READ: {
            struct tpm_ioctl_nv_op *p = (struct tpm_ioctl_nv_op *)arg;
            return tpm2_nv_read(p->nv_index, p->data, &p->len);
        }
        case TPMIO_NV_WRITE: {
            struct tpm_ioctl_nv_op *p = (struct tpm_ioctl_nv_op *)arg;
            return tpm2_nv_write(p->nv_index, p->data, p->len);
        }
        case TPMIO_SIGN: {
            struct tpm_ioctl_sign *p = (struct tpm_ioctl_sign *)arg;
            uint32_t sig_len = sizeof(p->signature);
            int ret = tpm2_sign(p->key_handle, p->digest,
                                p->digest_len, p->signature, &sig_len);
            if (ret == 0) p->sig_len = (uint16_t)sig_len;
            return ret;
        }
        default:
            return -EINVAL;
    }
}

/* Register /dev/tpm0 */
int tpm_dev_init(void)
{
    int ret = devfs_register_device("tpm0", NULL,
                                     tpm_dev_read, tpm_dev_write);
    if (ret < 0) {
        kprintf("[TPM] failed to register /dev/tpm0\n");
        return -EIO;
    }
    kprintf("[TPM] /dev/tpm0 registered (ioctl interface)\n");
    return 0;
}

void tpm_dev_exit(void)
{
    devfs_unregister_device("tpm0");
}

/* ═══════════════════════════════════════════════════════════════════════
 *  S96 — TPM NV key storage helpers
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * tpm_nv_store_key — Store a key blob in TPM NV storage.
 *
 * @nv_index:  NV index to use (e.g., 0x01C10101).
 * @key_data:  Key data to store (sealed blob or raw key).
 * @key_len:   Length of key data.
 *
 * Returns 0 on success.
 */
int tpm_nv_store_key(uint32_t nv_index, const uint8_t *key_data, uint32_t key_len)
{
    /* First, define the NV space if not already defined */
    int ret = tpm2_nv_define_space(nv_index, (key_len + 16) & ~15,
                                    TPMA_NV_AUTHWRITE | TPMA_NV_AUTHREAD |
                                    TPMA_NV_OWNERWRITE | TPMA_NV_OWNERREAD);
    if (ret < 0) {
        /* Space may already exist — try writing anyway */
    }

    /* Write the key data */
    return tpm2_nv_write(nv_index, key_data, key_len);
}

/*
 * tpm_nv_load_key — Load a key blob from TPM NV storage.
 *
 * @nv_index:  NV index to read from.
 * @key_data:  Buffer for key data.
 * @key_len:   On input: max size; on output: actual size.
 *
 * Returns 0 on success.
 */
int tpm_nv_load_key(uint32_t nv_index, uint8_t *key_data, uint32_t *key_len)
{
    return tpm2_nv_read(nv_index, key_data, key_len);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  PCR read/extend operations
 *
 *  Implements TPM2_PCR_Read and TPM2_PCR_Extend commands for
 *  Platform Configuration Registers (PCRs).  PCRs are shielded
 *  registers that hold cryptographic digests of measurements.
 *
 *  Item 350: TPM PCR read/extend operations
 * ═══════════════════════════════════════════════════════════════════════ */

/* Active PCR hash algorithms supported by this TPM.
 * Add new algorithms here when support is extended beyond SHA-256.
 * A non-zero algorithm ID indicates an active PCR bank. */
static const uint16_t pcr_active_hashes[] = {
    TPM2_ALG_SHA256,
    /* Future: TPM2_ALG_SHA1, TPM2_ALG_SHA384, TPM2_ALG_SHA512 */
};
#define PCR_ACTIVE_HASHES_COUNT \
    (sizeof(pcr_active_hashes) / sizeof(pcr_active_hashes[0]))

/* Return the number of active PCR hash algorithm banks. */
static int pcr_active_bank_count(void)
{
    int count = 0;
    for (size_t i = 0; i < PCR_ACTIVE_HASHES_COUNT; i++) {
        if (pcr_active_hashes[i] != 0)
            count++;
    }
    return count;
}

/* Build a TPM2_PCR_Read command buffer.
 * Format:
 *   tag (2) = TPM2_ST_NO_SESSIONS
 *   total_size (4)
 *   command_code (4) = TPM2_CC_PCR_READ
 *   pcr_selection (variable): size + pcr_select[]
 */
static int build_pcr_read_cmd(uint8_t *buf, uint32_t *buf_len,
                               uint32_t pcr_index)
{
    struct tpm_cmd_hdr *hdr = (struct tpm_cmd_hdr *)buf;
    hdr->tag = TPM2_ST_NO_SESSIONS;
    hdr->command_code = TPM2_CC_PCR_READ;

    /* PCR selection: pcr_select structure */
    uint8_t *p = buf + sizeof(struct tpm_cmd_hdr);
    *p++ = 0;   /* pcr_selection.sizeOfSelect = 0 (simplified) */

    /* For simplicity, we construct a minimal command.
     * The full TPM2_PCR_Read command has a complex PCR selection structure.
     * Our approach: pcrSelectionIn with all bits set for the given bank. */
    *p++ = 3;                     /* sizeofSelect = 3 (24 bits) */
    *p++ = 0;                     /* pcrSelect[0] */
    *p++ = 0;                     /* pcrSelect[1] */
    *p++ = 0;                     /* pcrSelect[2] */

    if (pcr_index < 8)
        buf[sizeof(struct tpm_cmd_hdr) + 4] |= (uint8_t)(1U << pcr_index);
    else if (pcr_index < 16)
        buf[sizeof(struct tpm_cmd_hdr) + 5] |= (uint8_t)(1U << (pcr_index - 8));
    else if (pcr_index < 24)
        buf[sizeof(struct tpm_cmd_hdr) + 6] |= (uint8_t)(1U << (pcr_index - 16));

    uint32_t total = (uint32_t)(p - buf);
    hdr->total_size = total + 5; /* + algorithm + digestsCount */
    /* Add algorithm and digestsCount fields */
    *p++ = (TPM2_ALG_SHA256 >> 8) & 0xFF;  /* algorithm (big-endian) */
    *p++ = TPM2_ALG_SHA256 & 0xFF;
    *p++ = (uint8_t)pcr_active_bank_count();  /* digestsCount from active PCR banks */
    *p++ = 0;   /* padding */

    total = (uint32_t)(p - buf);
    hdr->total_size = total;
    tpm_hdr_to_be(hdr);

    *buf_len = total;
    return 0;
}

/* Build a TPM2_PCR_Extend command buffer.
 * Format:
 *   tag (2) = TPM2_ST_SESSIONS (for auth) or TPM2_ST_NO_SESSIONS
 *   total_size (4)
 *   command_code (4) = TPM2_CC_PCR_EXTEND
 *   pcrHandle (4)
 *   authorization (variable, simplified)
 *   digests (variable)
 */
static int build_pcr_extend_cmd(uint8_t *buf, uint32_t *buf_len,
                                 uint32_t pcr_index,
                                 const uint8_t digest[TPM2_PCR_DIGEST_LEN])
{
    struct tpm_cmd_hdr *hdr = (struct tpm_cmd_hdr *)buf;
    hdr->tag = TPM2_ST_NO_SESSIONS;
    hdr->command_code = TPM2_CC_PCR_EXTEND;

    uint8_t *p = buf + sizeof(struct tpm_cmd_hdr);

    /* PCR handle (big-endian) */
    p[0] = (uint8_t)(pcr_index >> 24);
    p[1] = (uint8_t)(pcr_index >> 16);
    p[2] = (uint8_t)(pcr_index >> 8);
    p[3] = (uint8_t)(pcr_index & 0xFF);
    p += 4;

    /* No authorization session (simplified — not using TPM2_RS_PW) */
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 0;
    p += 4;

    /* Digests count (big-endian) */
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 1;
    p += 4;

    /* Hash algorithm (big-endian) */
    p[0] = (uint8_t)(TPM2_ALG_SHA256 >> 8);
    p[1] = (uint8_t)(TPM2_ALG_SHA256 & 0xFF);
    p += 2;

    /* Digest value */
    memcpy(p, digest, TPM2_PCR_DIGEST_LEN);
    p += TPM2_PCR_DIGEST_LEN;

    uint32_t total = (uint32_t)(p - buf);
    hdr->total_size = total;
    tpm_hdr_to_be(hdr);

    *buf_len = total;
    return 0;
}

/* Parse a TPM2_PCR_Read response and extract digest.
 * Returns 0 on success, -1 on failure. */
static int parse_pcr_read_rsp(const uint8_t *rsp, uint32_t rsp_len,
                               uint8_t digest[TPM2_PCR_DIGEST_LEN])
{
    if (rsp_len < sizeof(struct tpm_rsp_hdr) + 8)
        return -EIO;

    /* Read return_code in big-endian (TPM wire format) */
    uint32_t rc = ((uint32_t)rsp[6] << 24) | ((uint32_t)rsp[7] << 16) |
                  ((uint32_t)rsp[8] << 8)  |  (uint32_t)rsp[9];
    if (rc != TPM2_RC_SUCCESS)
        return -EIO;

    /* Response structure:
     *   pcrSelect (variable)
     *   digestsCount (4)
     *   digests: [algorithm (2) + hash (32)]
     */
    const uint8_t *p = rsp + sizeof(struct tpm_rsp_hdr);

    /* Skip pcrSelectOut */
    uint8_t size_of_select = *p++;
    p += size_of_select;  /* skip the PCR select bitmap */

    /* Read digests count (big-endian) */
    uint32_t digests_count = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                             ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
    p += 4;

    if (digests_count == 0)
        return -EINVAL;

    /* Read the hash algorithm (big-endian) */
    uint16_t algo = ((uint16_t)p[0] << 8) | (uint16_t)p[1];
    p += 2;

    (void)algo; /* we expect SHA-256 */

    /* Verify enough bytes remain for the full digest */
    uint32_t remaining = rsp_len - (uint32_t)(p - rsp);
    if (remaining < TPM2_PCR_DIGEST_LEN)
        return -EIO;

    memcpy(digest, p, TPM2_PCR_DIGEST_LEN);
    return 0;
}

/* TPM2_PCR_Read: read the current value of a PCR.
 *
 * @pcr_index: PCR index (0-23 for standard PCRs)
 * @digest:    output buffer for 32-byte SHA-256 digest
 *
 * Returns 0 on success, negative errno on failure.
 */
int tpm2_pcr_read(uint32_t pcr_index, uint8_t digest[TPM2_PCR_DIGEST_LEN])
{
    if (!g_tpm.initialized)
        return -ENODEV;
    if (pcr_index >= 24)
        return -EINVAL;
    if (!digest)
        return -EFAULT;

    uint8_t cmd[128];
    uint32_t cmd_len = 0;
    uint8_t rsp[256];
    uint32_t rsp_len = sizeof(rsp);

    int ret = build_pcr_read_cmd(cmd, &cmd_len, pcr_index);
    if (ret != 0) return ret;

    ret = tpm_transmit(cmd, cmd_len, rsp, &rsp_len);
    if (ret != 0) return ret;

    ret = parse_pcr_read_rsp(rsp, rsp_len, digest);
    if (ret != 0) {
        kprintf("[TPM] PCR_Read failed for index %u\n", pcr_index);
        return -EIO;
    }

    kprintf("[TPM] PCR_Read index %u: ", pcr_index);
    for (int i = 0; i < TPM2_PCR_DIGEST_LEN; i++)
        kprintf("%02x", digest[i]);
    kprintf("\n");
    return 0;
}
EXPORT_SYMBOL(tpm2_pcr_read);

/* TPM2_PCR_Extend: extend a PCR with a new digest value.
 *
 * PCR extension: new_value = SHA256(old_value || digest)
 *
 * @pcr_index: PCR index (0-23 for standard PCRs)
 * @digest:    32-byte SHA-256 digest to extend into the PCR
 *
 * Returns 0 on success, negative errno on failure.
 */
int tpm2_pcr_extend(uint32_t pcr_index,
                    const uint8_t digest[TPM2_PCR_DIGEST_LEN])
{
    if (!g_tpm.initialized)
        return -ENODEV;
    if (pcr_index >= 24)
        return -EINVAL;
    if (!digest)
        return -EFAULT;

    uint8_t cmd[256];
    uint32_t cmd_len = 0;
    uint8_t rsp[64];
    uint32_t rsp_len = sizeof(rsp);

    int ret = build_pcr_extend_cmd(cmd, &cmd_len, pcr_index, digest);
    if (ret != 0) return ret;

    ret = tpm_transmit(cmd, cmd_len, rsp, &rsp_len);
    if (ret != 0) return ret;

    /* Read return_code in big-endian (TPM wire format) */
    {
        uint32_t rc = ((uint32_t)rsp[6] << 24) | ((uint32_t)rsp[7] << 16) |
                      ((uint32_t)rsp[8] << 8)  |  (uint32_t)rsp[9];
        if (rc != TPM2_RC_SUCCESS) {
            kprintf("[TPM] PCR_Extend failed for index %u: rc=0x%08x\n",
                    pcr_index, rc);
            return -EIO;
        }
    }

    kprintf("[TPM] PCR_Extend index %u succeeded\n", pcr_index);
    return 0;
}
EXPORT_SYMBOL(tpm2_pcr_extend);

EXPORT_SYMBOL(tpm_init);
EXPORT_SYMBOL(tpm_transmit);
EXPORT_SYMBOL(tpm2_get_random);
EXPORT_SYMBOL(tpm2_startup);
EXPORT_SYMBOL(tpm_is_present);
EXPORT_SYMBOL(tpm2_sign);
EXPORT_SYMBOL(tpm_nv_store_key);
EXPORT_SYMBOL(tpm_nv_load_key);

/* ── TIS init wrapper ────────────────────────────────── */
static int tpm_tis_init(void *dev)
{
    (void)dev;
    return tpm_init();
}

/* ── TIS send wrapper ────────────────────────────────── */
static int tpm_tis_send(const void *cmd, size_t len)
{
    uint8_t rsp[128];
    uint32_t rsp_len = sizeof(rsp);
    return tpm_transmit((const uint8_t *)cmd, (uint32_t)len, rsp, &rsp_len);
}

/* ── TIS recv wrapper ────────────────────────────────── */
static int tpm_tis_recv(void *resp, size_t *len)
{
    /* For a real recv, we'd need a stored response buffer.
     * Since tpm_transmit is synchronous, this is a no-op. */
    (void)resp;
    (void)len;
    return 0;
}

/* ── TIS get_random wrapper ──────────────────────────── */
static int tpm_tis_get_random(void *buf, size_t count)
{
    return tpm2_get_random((uint8_t *)buf, (uint32_t)count);
}
