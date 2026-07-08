/*
 * ipmi_kcs.c — IPMI KCS (Keyboard Controller Style) interface driver
 *
 * Implements the KCS state machine for communicating with a BMC
 * (Baseboard Management Controller) over the standard KCS I/O ports.
 *
 * The KCS interface uses two I/O registers:
 *   DATA  (base + 0) — read/write data byte
 *   CMD   (base + 1) — write command, read status
 *
 * KCS state machine states:
 *   IDLE  → WRITE_START → WRITE → WRITE_END → READ → IDLE
 *   Error state on protocol violation.
 *
 * Reference: IPMI v2.0 spec, Section 9.5 — KCS Interface.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "ipmi.h"
#include "io.h"
#include "printf.h"
#include "string.h"
#include "timer.h"

/* ── KCS register accessors ────────────────────────────────────────── */

static inline uint8_t kcs_read_data(uint16_t base)
{
    return inb(base + IPMI_KCS_DATA);
}

static inline void kcs_write_data(uint16_t base, uint8_t val)
{
    outb(base + IPMI_KCS_DATA, val);
}

static inline uint8_t kcs_read_status(uint16_t base)
{
    return inb(base + IPMI_KCS_STATUS);
}

static inline void kcs_write_cmd(uint16_t base, uint8_t val)
{
    outb(base + IPMI_KCS_CMD, val);
}

/* ── Busy-wait for state transition with timeout ──────────────────── */

/*
 * Wait for the KCS controller to be ready (IBF cleared, not in error).
 * Returns 0 on success, -1 on timeout.
 */
static int kcs_wait_ibf(uint16_t base, int timeout_us)
{
    for (int i = 0; i < timeout_us; i++) {
        uint8_t sts = kcs_read_status(base);
        if (!(sts & IPMI_KCS_STS_IBF))
            return 0;
        /* If we have a timing source, use it; otherwise just loop */
        for (volatile int j = 0; j < 10; j++)
            __asm__ volatile("pause");
    }
    return -1; /* timeout */
}

/*
 * Wait for OBF (Output Buffer Full) — data is ready to read.
 * Returns 0 on success, -1 on timeout.
 */
static int kcs_wait_obf(uint16_t base, int timeout_us)
{
    for (int i = 0; i < timeout_us; i++) {
        uint8_t sts = kcs_read_status(base);
        if (sts & IPMI_KCS_STS_OBF)
            return 0;
        /* Check for error state: both SMS bits set */
        if ((sts & IPMI_KCS_STS_SMS) == IPMI_KCS_STS_SMS)
            return -1;
        for (volatile int j = 0; j < 10; j++)
            __asm__ volatile("pause");
    }
    return -1; /* timeout */
}

/* Abort any pending KCS operation and return to IDLE state */
static void kcs_abort(uint16_t base)
{
    /* Write ABORT command */
    kcs_write_cmd(base, 0x60); /* Get Status / abort */
    /* Read and discard status until idle */
    for (int i = 0; i < 100; i++) {
        uint8_t sts = kcs_read_status(base);
        if (!(sts & IPMI_KCS_STS_OBF) && !(sts & IPMI_KCS_STS_IBF))
            break;
        /* Clear OBF by reading data */
        if (sts & IPMI_KCS_STS_OBF)
            (void)kcs_read_data(base);
        for (volatile int j = 0; j < 100; j++)
            __asm__ volatile("pause");
    }
}

/* ── KCS message transfer ─────────────────────────────────────────── */

/*
 * Send an IPMI message (netfn + cmd + data) and receive the response
 * over the KCS interface.
 *
 * KCS write sequence:
 *   1. WRITE_START command (0x61)
 *   2. Write netfn|0 (request network function)
 *   3. For each data byte: write data byte
 *   4. WRITE_END command (0x62)
 *   5. Write checksum (1's complement of netfn + data bytes)
 *   6. Read response (OBF → status, then data bytes + checksum)
 *
 * Returns 0 on success, -1 on error.
 */
int ipmi_send_cmd(struct ipmi_msg *msg)
{
    if (!msg)
        return -1;

    uint16_t base = IPMI_KCS_BASE;

    /* ── Probe: check that the KCS interface exists ── */
    uint8_t probe = kcs_read_status(base);
    if (probe == 0xFF)
        return -1; /* no device at this base address */

    /* Abort any stale transaction */
    kcs_abort(base);

    /* ── Step 1: WRITE_START ────────────────────────────────────── */
    kcs_write_cmd(base, IPMI_KCS_WRITE_START);
    if (kcs_wait_ibf(base, 5000) < 0)
        return -1;

    /* ── Step 2: Write request NetFn / LUN ──────────────────────── */
    /* NetFn is in upper bits, LUN in lower 2 bits; we use LUN=0 */
    uint8_t req_hdr = (msg->netfn << 2) | 0x00; /* NetFn << 2 | LUN */
    kcs_write_data(base, req_hdr);
    if (kcs_wait_ibf(base, 5000) < 0)
        return -1;

    /* ── Step 3: Write command byte ─────────────────────────────── */
    kcs_write_data(base, msg->cmd);
    if (kcs_wait_ibf(base, 5000) < 0)
        return -1;

    /* ── Step 4: Write data bytes ───────────────────────────────── */
    for (int i = 0; i < msg->data_len; i++) {
        kcs_write_data(base, msg->data[i]);
        if (kcs_wait_ibf(base, 5000) < 0)
            return -1;
    }

    /* ── Step 5: WRITE_END ──────────────────────────────────────── */
    kcs_write_cmd(base, IPMI_KCS_WRITE_END);
    if (kcs_wait_ibf(base, 5000) < 0)
        return -1;

    /* ── Step 6: Write 1's complement checksum ──────────────────── */
    {
        uint8_t cksum = (uint8_t)(~(req_hdr + msg->cmd));
        for (int i = 0; i < msg->data_len; i++)
            cksum = (uint8_t)(cksum + msg->data[i]);
        cksum = (uint8_t)(~cksum);
        kcs_write_data(base, cksum);
    }

    if (kcs_wait_obf(base, 50000) < 0) {
        kcs_abort(base);
        return -1;
    }

    /* ── Step 7: Read response ──────────────────────────────────── */

    /* Read response NetFn (OBF should be set) */
    {
        uint8_t sts = kcs_read_status(base);
        if (!(sts & IPMI_KCS_STS_OBF)) {
            kcs_abort(base);
            return -1;
        }
    }
    uint8_t rsp_hdr = kcs_read_data(base);  /* clears OBF */

    /* The response NetFn should be the request NetFn | 1 (response bit) */
    if ((rsp_hdr >> 2) != ((msg->netfn | 1) & 0x3F)) {
        kcs_abort(base);
        return -1;
    }

    /* Read command byte (echoes our command) */
    if (kcs_wait_obf(base, 50000) < 0) { kcs_abort(base); return -1; }
    uint8_t rsp_cmd = kcs_read_data(base);
    if (rsp_cmd != msg->cmd) {
        kcs_abort(base);
        return -1;
    }

    /* Read completion code */
    if (kcs_wait_obf(base, 50000) < 0) { kcs_abort(base); return -1; }
    msg->completion_code = kcs_read_data(base);

    /* Read response data */
    size_t rsp_idx = 0;
    while (rsp_idx < sizeof(msg->rsp)) {
        if (kcs_wait_obf(base, 50000) < 0)
            break;
        uint8_t sts = kcs_read_status(base);
        if (!(sts & IPMI_KCS_STS_OBF))
            break;
        uint8_t byte = kcs_read_data(base);

        /* The last byte is the checksum; we don't store it */
        /* OBF cleared — check if we're done by seeing if SMS went IDLE */
        uint8_t new_sts = kcs_read_status(base);
        if (!(new_sts & IPMI_KCS_STS_OBF) && !(new_sts & IPMI_KCS_STS_IBF)) {
            /* KCS is back to IDLE — this was the last data byte */
            msg->rsp[rsp_idx++] = byte;
            break;
        }
        msg->rsp[rsp_idx++] = byte;
    }
    msg->rsp_len = rsp_idx;

    return 0;
}

/* ── High-level IPMI commands ─────────────────────────────────────── */

/*
 * Detect whether an IPMI BMC is present at the KCS base address.
 * Attempts to read the device ID; returns 1 if present, 0 if not.
 */
int ipmi_is_present(void)
{
    uint16_t base = IPMI_KCS_BASE;

    /* Quick probe: read status, check for floating bus */
    uint8_t sts = kcs_read_status(base);
    if (sts == 0xFF || sts == 0x00)
        return 0;

    /* Try to get the device ID to confirm BMC presence */
    struct ipmi_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.netfn    = IPMI_NETFN_APP;
    msg.cmd      = IPMI_CMD_GET_DEVICE_ID;
    msg.data_len = 0;

    if (ipmi_send_cmd(&msg) < 0)
        return 0;

    return (msg.completion_code == 0); /* CC 0 = success */
}

/*
 * Get the IPMI device ID and firmware revision.
 * @dev_id receives the device ID byte.
 * @rev receives the firmware revision byte (major.minor).
 * Returns 0 on success, -1 on error.
 */
int ipmi_get_device_id(uint8_t *dev_id, uint8_t *rev)
{
    if (!dev_id || !rev)
        return -1;

    struct ipmi_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.netfn    = IPMI_NETFN_APP;
    msg.cmd      = IPMI_CMD_GET_DEVICE_ID;
    msg.data_len = 0;

    if (ipmi_send_cmd(&msg) < 0)
        return -1;
    if (msg.completion_code != 0)
        return -1;
    if (msg.rsp_len < 3)
        return -1;

    /* Response: dev_id (byte 0), rev (byte 1), fw_rev[12] */ ;
    *dev_id = msg.rsp[0];
    *rev    = msg.rsp[1];
    return 0;
}

/*
 * Get chassis power state.
 * @power_state receives the power state bits (bit 0 = power on).
 * Returns 0 on success, -1 on error.
 */
int ipmi_chassis_status(uint8_t *power_state)
{
    if (!power_state)
        return -1;

    struct ipmi_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.netfn    = IPMI_NETFN_CHASSIS;
    msg.cmd      = IPMI_CMD_GET_CHASSIS_STATUS;
    msg.data_len = 0;

    if (ipmi_send_cmd(&msg) < 0)
        return -1;
    if (msg.completion_code != 0)
        return -1;
    if (msg.rsp_len < 1)
        return -1;

    *power_state = msg.rsp[0];
    return 0;
}

/*
 * Control chassis power (on/off/cycle/reset).
 * @power_action: IPMI_POWER_OFF, IPMI_POWER_ON, IPMI_POWER_CYCLE, IPMI_POWER_RESET
 * Returns 0 on success, -1 on error.
 */
int ipmi_chassis_control(int power_action)
{
    struct ipmi_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.netfn    = IPMI_NETFN_CHASSIS;
    msg.cmd      = IPMI_CMD_CHASSIS_CONTROL;
    msg.data[0]  = (uint8_t)power_action;
    msg.data_len = 1;

    if (ipmi_send_cmd(&msg) < 0)
        return -1;

    return (msg.completion_code == 0) ? 0 : -1;
}

/* ── Initialisation ───────────────────────────────────────────────── */

static int ipmi_initialised = 0;
static struct ipmi_device g_ipmi_dev;

int ipmi_init(void)
{
    if (ipmi_initialised)
        return 0;

    memset(&g_ipmi_dev, 0, sizeof(g_ipmi_dev));
    g_ipmi_dev.base_addr = IPMI_KCS_BASE;
    g_ipmi_dev.if_type   = IPMI_IF_KCS;

    if (!ipmi_is_present()) {
        kprintf("[--] IPMI: no BMC detected at KCS base 0x%x\\n",
                (unsigned int)IPMI_KCS_BASE);
        return -1;
    }

    uint8_t dev_id = 0, rev = 0;
    if (ipmi_get_device_id(&dev_id, &rev) == 0) {
        g_ipmi_dev.present    = 1;
        g_ipmi_dev.bmc_version = dev_id;
        g_ipmi_dev.fw_rev1    = rev;
        kprintf("[OK] IPMI: BMC detected (dev_id=0x%02x, rev=%d.%d)\\n",
                (unsigned int)dev_id, (rev >> 4) & 0x0F, rev & 0x0F);
    } else {
        /* Device ID read failed — BMC might be in boot mode */
        g_ipmi_dev.present = 1;
        kprintf("[OK] IPMI: BMC present (KCS at 0x%x)\\n",
                (unsigned int)IPMI_KCS_BASE);
    }

    ipmi_initialised = 1;
    return 0;
}

int ipmi_is_initialised(void)
{
    return ipmi_initialised;
}

const struct ipmi_device *ipmi_get_device(void)
{
    if (!ipmi_initialised)
        return NULL;
    return &g_ipmi_dev;
}

/* ── Stub: ipmi_kcs_init ─────────────────────────────── */
static int ipmi_kcs_init(void)
{
    kprintf("[ipmi] ipmi_kcs_init: not yet implemented\n");
    return 0;
}
/* ── Stub: ipmi_kcs_send ─────────────────────────────── */
static int ipmi_kcs_send(const void *msg, size_t len)
{
    (void)msg;
    (void)len;
    kprintf("[ipmi] ipmi_kcs_send: not yet implemented\n");
    return 0;
}
/* ── Stub: ipmi_kcs_recv ─────────────────────────────── */
static int ipmi_kcs_recv(void *msg, size_t *len)
{
    (void)msg;
    (void)len;
    kprintf("[ipmi] ipmi_kcs_recv: not yet implemented\n");
    return 0;
}
/* ── Stub: ipmi_kcs_poll ─────────────────────────────── */
static int ipmi_kcs_poll(void)
{
    kprintf("[ipmi] ipmi_kcs_poll: not yet implemented\n");
    return 0;
}
