#ifndef TPM_H
#define TPM_H

/*
 * tpm.h — TPM 2.0 register and command definitions (Item 349/350)
 *
 * TIS (Trusted Interface Specification) for TPM 2.0:
 *   - MMIO at 0xFED40000 (default TIS base on x86)
 *   - FIFO interface for command/response transport
 *   - Locality 0 (default locality for standard operation)
 *
 * Reference: TCG PC Client Platform TPM Profile (PTP) Specification,
 *            TPM 2.0 Part 1: Architecture
 */

#include "types.h"

/* ═══════════════════════════════════════════════════════════════════════
 *  TIS Register Offsets (Locality 0, base + offset)
 * ═══════════════════════════════════════════════════════════════════════ */

#define TIS_ACCESS           0x0000  /* 1 byte: access/status */
#define TIS_INT_ENABLE       0x0008  /* 4 bytes: interrupt enable */
#define TIS_INT_VECTOR       0x000C  /* 4 bytes: interrupt vector */
#define TIS_INT_STATUS       0x0010  /* 4 bytes: interrupt status */
#define TIS_INTF_CAPABILITY  0x0014  /* 4 bytes: interface capability */
#define TIS_STS              0x0018  /* 4 bytes: status (burst count + state) */
#define TIS_DATA_FIFO        0x0024  /* 4 bytes: data FIFO (read/write) */
#define TIS_INTERFACE_ID     0x0030  /* 4 bytes: interface ID */
#define TIS_XDATA_FIFO       0x0040  /* 4 bytes: extended data FIFO */
#define TIS_DID_VID          0x0F00  /* 4 bytes: device + vendor ID */
#define TIS_RID              0x0F04  /* 1 byte:  revision ID */

/* ── TIS_ACCESS bits ───────────────────────────────────────────────── */
#define TIS_ACC_ACTIVE_LOC    (1U << 5)  /* Locality is active */
#define TIS_ACC_SEIZE         (1U << 4)  /* Seize locality (interrupt-based) */
#define TIS_ACC_REQ_USE       (1U << 1)  /* Request locality use */
#define TIS_ACC_ESTABLISH     (1U << 0)  /* Locality established */

/* ── TIS_STS bits ──────────────────────────────────────────────────── */
#define TIS_STS_RESET_EST     (1U << 6)  /* Reset establishment (cancel) */
#define TIS_STS_CMD_READY     (1U << 5)  /* Command ready (ready state) */
#define TIS_STS_INT_VALID     (1U << 4)  /* Interrupt valid */
#define TIS_STS_VALID         (1U << 3)  /* Status valid (write complete) */
#define TIS_STS_DATA_AVAIL    (1U << 2)  /* Data available (response ready) */
#define TIS_STS_EXPECT        (1U << 1)  /* Expect more data */
#define TIS_STS_GO            (1U << 0)  /* Go (execute command) */

/* Burst count mask (upper bytes of STS) */
#define TIS_STS_BURST_COUNT_MASK  0xFFFF0000ULL
#define TIS_STS_BURST_COUNT_SHIFT 16

/* ── TPM command header structure (simplified) ─────────────────────── */
struct tpm_cmd_hdr {
    uint16_t tag;          /* TPM_ST_NO_SESSIONS or TPM_ST_SESSIONS */
    uint32_t total_size;   /* total command size including header */
    uint32_t command_code; /* TPM2_CC_* command code */
} __attribute__((packed));

struct tpm_rsp_hdr {
    uint16_t tag;
    uint32_t total_size;
    uint32_t return_code;  /* TPM2_RC_* */
} __attribute__((packed));

/* ── TPM command codes ─────────────────────────────────────────────── */
#define TPM2_CC_GET_RANDOM      0x0000017B
#define TPM2_CC_PCR_READ        0x0000017E
#define TPM2_CC_PCR_EXTEND     0x00000182
#define TPM2_CC_GET_CAPABILITY  0x0000017A
#define TPM2_CC_STARTUP        0x00000144
#define TPM2_CC_SELF_TEST      0x00000143
#define TPM2_CC_NV_DEFINE_SPACE 0x0000012A
#define TPM2_CC_NV_WRITE       0x00000137
#define TPM2_CC_NV_READ        0x0000014E
#define TPM2_CC_QUOTE          0x00000158
#define TPM2_CC_CREATE         0x00000156
#define TPM2_CC_CREATE_LOADED   0x0000015F  /* CreateLoaded */
#define TPM2_CC_LOAD           0x00000157
#define TPM2_CC_UNSEAL         0x0000015E
#define TPM2_CC_CONTEXT_LOAD   0x00000161
#define TPM2_CC_CONTEXT_SAVE   0x00000162
#define TPM2_CC_FLUSH_CONTEXT  0x00000165
#define TPM2_CC_SIGN           0x0000015D

/* ── TPM response codes ────────────────────────────────────────────── */
#define TPM2_RC_SUCCESS        0x00000000
#define TPM2_RC_FAILURE        0x00000101

/* ── TPM command tags ──────────────────────────────────────────────── */
#define TPM2_ST_NO_SESSIONS    0x8001
#define TPM2_ST_SESSIONS       0x8002

/* ── TPM PCR constants ────────────────────────────────────────────── */
#define TPM2_MAX_PCR_BANKS     5
#define TPM2_PCR_SELECT_MAX    (3 + 1)  /* 24 bytes select + size */
#define TPM2_PCR_DIGEST_LEN   32        /* SHA-256 digest length */

/* ── TPM handle types ────────────────────────────────────────────── */
#define TPM2_RH_NULL            0x40000007
#define TPM2_RH_OWNER           0x40000001
#define TPM2_RH_PLATFORM        0x4000000C
#define TPM2_RH_ENDORSEMENT     0x4000000B

/* ── TPM NV index attributes (simplified) ────────────────────────── */
#define TPM2_NT_ORDINARY        0x0
#define TPM2_NT_COUNTER         0x1
#define TPM2_NT_BITS            0x2
#define TPM2_NT_PIN_FAIL        0x4
#define TPM2_NT_PIN_PASS        0x8

#define TPMA_NV_AUTHWRITE       (1u << 0)
#define TPMA_NV_AUTHREAD        (1u << 1)
#define TPMA_NV_PPWRITE         (1u << 2)
#define TPMA_NV_PPREAD          (1u << 3)
#define TPMA_NV_PLATFORMCREATE  (1u << 10)
#define TPMA_NV_WRITTEN         (1u << 11)
#define TPMA_NV_OWNERWRITE      (1u << 12)
#define TPMA_NV_OWNERREAD       (1u << 13)

/* ── TPM algorithm identifiers ───────────────────────────────────── */
#define TPM2_ALG_SHA256         0x000B
#define TPM2_ALG_RSA            0x0001
#define TPM2_ALG_NULL           0x0010
#define TPM2_ALG_ECDSA          0x0018
#define TPM2_ALG_CFB            0x0042

/* ── Session / authorization constants ────────────────────────────── */
#define TPM2_RS_PW              0x40000009   /* Password auth session handle */
#define TPM2_SU_CLEAR         0x0000
#define TPM2_SU_STATE         0x0001

/* ── TPM interface constants ───────────────────────────────────────── */
#define TPM_TIS_BASE          0xFED40000ULL
#define TPM_TIS_SIZE          0x5000
#define TPM_TIMEOUT_A         750000    /* 750 ms in microseconds */
#define TPM_TIMEOUT_B         2000000   /* 2 s */
#define TPM_TIMEOUT_C         200000    /* 200 ms */
#define TPM_TIMEOUT_D         30000000  /* 30 s (for command execution) */
#define TPM_NUM_RETRIES       5

/* ── TIS State Machine ─────────────────────────────────────────────── */
enum tis_state {
    TIS_STATE_IDLE     = 0,
    TIS_STATE_READY    = 1,
    TIS_STATE_RECEIVE  = 2,
    TIS_STATE_EXECUTE  = 3,
    TIS_STATE_COMPLETE = 4,
};

/* ── TPM device descriptor ─────────────────────────────────────────── */
struct tpm_device {
    volatile uint8_t  *mmio_base;   /* virtual address of TIS MMIO region */
    uint16_t           vid;          /* vendor ID */
    uint16_t           did;          /* device ID */
    uint8_t            revision;
    int                initialized;
    enum tis_state     state;
};

/* ═══════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════ */

/* Initialize and probe for TPM */
int tpm_init(void);

/* Send a TPM command and receive response */
int tpm_transmit(const uint8_t *cmd, uint32_t cmd_len,
                 uint8_t *rsp, uint32_t *rsp_len);

/* High-level TPM operations */
int tpm2_get_random(uint8_t *buf, uint32_t count);
int tpm2_pcr_read(uint32_t pcr_index, uint8_t digest[TPM2_PCR_DIGEST_LEN]);
int tpm2_pcr_extend(uint32_t pcr_index,
                    const uint8_t digest[TPM2_PCR_DIGEST_LEN]);

/* S96 — TPM resource manager (context save/load) */
int tpm2_context_save(void *object_handle, uint8_t *out_buf, uint32_t *out_len);
int tpm2_context_load(const uint8_t *ctx_buf, uint32_t ctx_len,
                       uint32_t *loaded_handle);
int tpm2_flush_context(uint32_t handle);

/* S97 — TPM NV index operations */
int tpm2_nv_define_space(uint32_t nv_index, uint32_t data_size,
                          uint32_t nv_attributes);
int tpm2_nv_write(uint32_t nv_index, const uint8_t *data, uint32_t len);
int tpm2_nv_read(uint32_t nv_index, uint8_t *buf, uint32_t *len);

/* S98 — TPM attestation (Quote) */
int tpm2_quote(uint32_t pcr_index, const uint8_t *nonce, uint32_t nonce_len,
               uint8_t *attest_buf, uint32_t *attest_len);

/* S99 — TPM sealing (Create/Unseal) */
int tpm2_create(uint32_t parent_handle, const uint8_t *sealed_data,
                uint32_t sealed_len, const uint8_t *auth, uint32_t auth_len,
                uint8_t *priv_buf, uint32_t *priv_len,
                uint8_t *pub_buf, uint32_t *pub_len);
int tpm2_load(uint32_t parent_handle,
              const uint8_t *priv_buf, uint32_t priv_len,
              const uint8_t *pub_buf, uint32_t pub_len,
              uint32_t *loaded_handle);
int tpm2_unseal(uint32_t item_handle,
                uint8_t *data_buf, uint32_t *data_len);

/* S96 — TPM2_Sign */
int tpm2_sign(uint32_t key_handle, const uint8_t *digest, uint32_t digest_len,
              uint8_t *sig_buf, uint32_t *sig_len);

/* S96 — TPM NV key storage */
int tpm_nv_store_key(uint32_t nv_index, const uint8_t *key_data, uint32_t key_len);
int tpm_nv_load_key(uint32_t nv_index, uint8_t *key_data, uint32_t *key_len);

/* S96 — /dev/tpm0 device */
int tpm_dev_init(void);
void tpm_dev_exit(void);

#endif /* TPM_H */
