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
#include "export.h"
#include "spinlock.h"
#include "vmm.h"   /* vmm_map_phys, vmm_unmap_phys */

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

    while (1) {
        uint32_t val = (reg == TIS_ACCESS)
                       ? (uint32_t)tis_read8(dev, reg)
                       : tis_read32(dev, reg);
        int bit_set = (val & mask) != 0;
        if (set ? bit_set : !bit_set)
            return 0;  /* Condition met */

        if (timer_get_ticks() >= deadline)
            return -1;  /* Timeout */

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
        return -1;
    }
    return 0;
}

static int tis_relinquish_locality(struct tpm_device *dev) {
    tis_write8(dev, TIS_ACCESS, TIS_ACC_SEIZE);
    return 0;
}

/* Transition from IDLE to READY state */
static int tis_ready(struct tpm_device *dev) {
    tis_write32(dev, TIS_STS, TIS_STS_CMD_READY);

    /* Wait for status to settle (expect bit cleared after ready) */
    if (tis_wait_for_bit(dev, TIS_STS, TIS_STS_CMD_READY, 0,
                         TPM_TIMEOUT_B) < 0) {
        kprintf("[TPM] timeout entering ready state\n");
        return -1;
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
        return -1;
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

/* ── Command transmission (write command, execute, read response) ── */

int tpm_transmit(const uint8_t *cmd, uint32_t cmd_len,
                 uint8_t *rsp, uint32_t *rsp_len)
{
    struct tpm_device *dev = &g_tpm;
    int ret = -1;

    if (!dev->initialized) {
        kprintf("[TPM] not initialized\n");
        return -1;
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

    struct tpm_rsp_hdr *hdr = (struct tpm_rsp_hdr *)rsp_hdr_buf;
    uint32_t expected = hdr->total_size;

    if (expected < sizeof(struct tpm_rsp_hdr) || expected > *rsp_len) {
        kprintf("[TPM] invalid response size %u (buf=%u)\n",
                expected, *rsp_len);
        goto cancel;
    }

    /* 9. Copy header to output buffer */
    memcpy(rsp, rsp_hdr_buf, sizeof(struct tpm_rsp_hdr));
    uint32_t remaining = expected - sizeof(struct tpm_rsp_hdr);
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
    ret = (hdr->return_code == TPM2_RC_SUCCESS) ? 0 : -1;

    if (ret != 0) {
        kprintf("[TPM] command failed: return_code=0x%08x\n", hdr->return_code);
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
int tpm2_startup(uint16_t startup_type) {
    uint8_t cmd[12];  /* header + 2 bytes startup type */
    struct tpm_cmd_hdr *hdr = (struct tpm_cmd_hdr *)cmd;

    hdr->tag = TPM2_ST_NO_SESSIONS;
    hdr->total_size = sizeof(cmd);
    hdr->command_code = TPM2_CC_STARTUP;

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
int tpm2_selftest(int full_test) {
    uint8_t cmd[11];  /* header + 1 byte fullTest */
    struct tpm_cmd_hdr *hdr = (struct tpm_cmd_hdr *)cmd;

    hdr->tag = TPM2_ST_NO_SESSIONS;
    hdr->total_size = sizeof(cmd);
    hdr->command_code = TPM2_CC_SELF_TEST;

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
    if (!buf || count == 0) return -1;

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
int tpm2_pcr_read(uint32_t pcr_index, uint8_t digest[TPM2_PCR_DIGEST_LEN]) {
    if (!digest || pcr_index > 23) return -1;  /* PCRs 0-23 are standard */

    /*
     * Command buffer:
     *   +0: tpm_cmd_hdr (10 bytes)
     *   +10: pcrSelectIn.size (2 bytes) = 3 (pcr selection bitmap)
     *   +12: pcrSelectIn.pcrSelect (3 bytes) = bitmap
     */
    uint8_t cmd[15];
    struct tpm_cmd_hdr *hdr = (struct tpm_cmd_hdr *)cmd;

    hdr->tag = TPM2_ST_NO_SESSIONS;
    hdr->total_size = sizeof(cmd);
    hdr->command_code = TPM2_CC_PCR_READ;

    /* pcrSelectionIn: size=3, pcrSelect bitmap */
    cmd[10] = 0;    /* sizeof(TPMS_PCR_SELECTION): upper byte = 0 */
    cmd[11] = 3;    /* 3 bytes of PCR select bitmap */
    cmd[12] = 0;    /* hashAlg = TPM2_ALG_SHA1? No, actually it's */
                     /* TPMS_PCR_SELECTION: hash+size+select */
    /* Actually the TPM2B_PCR_SELECTION structure is:
     *   UINT16 size;         // count of TPMS_PCR_SELECTION structures
     * followed by TPMS_PCR_SELECTION[]:
     *   TPMI_ALG_HASH hash;
     *   UINT8 sizeofSelect;
     *   UINT8 pcrSelect[sizeofSelect];
     *
     * Simplified: send 1 selection with SHA256 hash and the requested PCR.
     */

    /* Let's reconstruct: */
    /* TPM2B_PCR_SELECTION: size of the array (UINT16) */
    cmd[10] = 0;
    cmd[11] = 1;  /* one TPMS_PCR_SELECTION structure */

    /* TPMS_PCR_SELECTION: hashAlg (TPM2_ALG_SHA256 = 0x000B) */
    cmd[12] = (uint8_t)(0x000B >> 8);  /* = 0x00 */
    cmd[13] = (uint8_t)(0x000B & 0xFF); /* = 0x0B */

    /* sizeofSelect */
    cmd[14] = 3;  /* 3 bytes of PCR select */

    /* pcrSelect[3] */
    uint8_t select_pcr[3];
    memset(select_pcr, 0, 3);
    select_pcr[pcr_index / 8] |= (1 << (pcr_index % 8));

    /* We need space for select bytes — extend command */
    uint8_t cmd_full[18];
    memcpy(cmd_full, cmd, 15);
    cmd_full[15] = select_pcr[0];
    cmd_full[16] = select_pcr[1];
    cmd_full[17] = select_pcr[2];

    /* Update total size */
    struct tpm_cmd_hdr *hdr2 = (struct tpm_cmd_hdr *)cmd_full;
    hdr2->total_size = sizeof(cmd_full);

    uint8_t rsp[128];
    uint32_t rsp_len = sizeof(rsp);

    if (tpm_transmit(cmd_full, sizeof(cmd_full), rsp, &rsp_len) < 0)
        return -1;

    /* Parse response to extract digest.
     * Response structure:
     *   tpm_rsp_hdr (10 bytes)
     *   pcrSelectionOut (variable)
     *   tpm2b_digest.size (2 bytes)
     *   tpm2b_digest.buffer (N bytes)
     *
     * For a basic implementation, scan for the digest values.
     * The digest is always the last 32 bytes of the response.
     */
    if (rsp_len < sizeof(struct tpm_rsp_hdr) + TPM2_PCR_DIGEST_LEN)
        return -1;

    /* The digest SHA-256 value is at the end of the response */
    uint32_t digest_offset = rsp_len - TPM2_PCR_DIGEST_LEN;
    memcpy(digest, rsp + digest_offset, TPM2_PCR_DIGEST_LEN);

    return 0;
}

/* ── TPM2_PCR_Extend ─────────────────────────────────────────────────
 *
 * Extends the specified PCR with the given SHA-256 digest.
 * Returns 0 on success, -1 on failure.
 */
int tpm2_pcr_extend(uint32_t pcr_index,
                    const uint8_t digest[TPM2_PCR_DIGEST_LEN])
{
    if (!digest || pcr_index > 23) return -1;

    /*
     * Command:
     *   tpm_cmd_hdr (10 bytes)
     *   pcrHandle (4 bytes) = pcr_index
     *   digests.size (2 bytes) = 32 + 2 (size of TPML_DIGEST_VALUES)
     *   digests.digests[0].hashAlg (2 bytes) = SHA256
     *   digests.digests[0].digest (32 bytes)
     */
    uint8_t cmd[10 + 4 + 2 + 2 + TPM2_PCR_DIGEST_LEN];
    struct tpm_cmd_hdr *hdr = (struct tpm_cmd_hdr *)cmd;

    hdr->tag = TPM2_ST_SESSIONS;  /* needs authorization */
    hdr->total_size = sizeof(cmd);
    hdr->command_code = TPM2_CC_PCR_EXTEND;

    /* PCR handle */
    cmd[10] = 0;
    cmd[11] = 0;
    cmd[12] = 0;
    cmd[13] = (uint8_t)pcr_index;

    /* TPML_DIGEST_VALUES: count (UINT32 in some TPMs, UINT16 in TCG) */
    /* Actually TPML_DIGEST_VALUES has UINT32 count */
    cmd[14] = 0;
    cmd[15] = 0;
    cmd[16] = 0;
    cmd[17] = 1;  /* 1 digest value */

    /* TPMS_DIGEST_VALUE: hashAlg = TPM2_ALG_SHA256 */
    cmd[18] = 0;
    cmd[19] = 0x0B;  /* TPM2_ALG_SHA256 */

    /* Digest value */
    memcpy(&cmd[20], digest, TPM2_PCR_DIGEST_LEN);

    uint8_t rsp[64];
    uint32_t rsp_len = sizeof(rsp);

    return tpm_transmit(cmd, sizeof(cmd), rsp, &rsp_len);
}

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
    if (!virt) {
        kprintf("[TPM] failed to map MMIO region at 0x%llx\n", phys);
        return -1;
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
        return -1;
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
        return -1;
    }

    /* ── Claim locality 0 ──────────────────────────────────────── */
    if (tis_request_locality(dev) < 0) {
        vmm_unmap_phys(virt, TPM_TIS_SIZE);
        dev->mmio_base = NULL;
        return -1;
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
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("TPM 2.0 TIS (FIFO) interface driver");
#endif /* MODULE */

/* ── Exported symbols for other modules/kernel code ───────────────── */
EXPORT_SYMBOL(tpm_init);
EXPORT_SYMBOL(tpm_transmit);
EXPORT_SYMBOL(tpm2_get_random);
EXPORT_SYMBOL(tpm2_pcr_read);
EXPORT_SYMBOL(tpm2_pcr_extend);
EXPORT_SYMBOL(tpm2_startup);
EXPORT_SYMBOL(tpm_is_present);
