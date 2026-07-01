#ifndef ATA_OPAL_H
#define ATA_OPAL_H

/*
 * ata_opal.h — TCG Opal Storage support over ATA TRUSTED SEND/RECEIVE.
 *
 * Provides the low-level ATA TRUSTED SEND (0x5B) and TRUSTED RECEIVE
 * (0x5C) command wrappers, Level 0 Discovery for self-encrypting
 * drives, and TCG Storage session management for Opal SSC drives.
 *
 * References:
 *   - TCG Storage Architecture Core Specification, v2.01
 *   - TCG Opal SSC v1.0 / v2.01
 *   - TCG Pyrite SSC v1.00
 *   - ATA/ATAPI-8 (ACS-3), T13/2161-D Revision 3
 */

#include "types.h"

/* ====================================================================
 *  ATA TRUSTED SEND / RECEIVE — Command Codes
 * ==================================================================== */

#define ATA_CMD_TRUSTED_SEND    0x5B  /* Send data to security protocol */
#define ATA_CMD_TRUSTED_RECV    0x5C  /* Receive data from security protocol */

/* ====================================================================
 *  TCG Security Protocol Identifiers
 * ==================================================================== */

#define TCG_SP_ATA_SECURITY      0x00  /* ATA Security feature set */
#define TCG_SP_TCG_STORAGE       0x01  /* TCG Storage (Opal / Pyrite / Ruby) */
#define TCG_SP_TCG_STORAGE_2     0x02  /* TCG Storage with hash extend */
#define TCG_SP_IEEE_1667         0x03  /* IEEE 1667 */
#define TCG_SP_VENDOR_SPECIFIC   0xEF  /* Vendor-specific protocols */

/* ====================================================================
 *  TCG Storage Level 0 Discovery — Feature Codes
 * ==================================================================== */

#define TCG_FEATURE_TCG_STORAGE  0x0001  /* TCG Storage feature descriptor */
#define TCG_FEATURE_OPAL_V1      0x0002  /* Opal SSC v1.0 */
#define TCG_FEATURE_OPAL_V2      0x0003  /* Opal SSC v2.01 */
#define TCG_FEATURE_PYRITE_V1    0x0004  /* Pyrite SSC v1.00 */
#define TCG_FEATURE_ENTERPRISE   0x0100  /* Enterprise SSC */
#define TCG_FEATURE_LBA_RANGE    0x0200  /* Locking LBA ranges */
#define TCG_FEATURE_LOCKING      0x0201  /* Locking SP info */
#define TCG_FEATURE_GEO         0x0202  /* Geometry reporting */
#define TCG_FEATURE_SINGLE_USER 0x0203  /* Single User Mode */
#define TCG_FEATURE_DATA_REM    0x0204  /* Data Removal mechanism */
#define TCG_FEATURE_ACE         0x0300  /* Additional ACE support */
#define TCG_FEATURE_NV_CACHE    0x0400  /* NV Cache support */
#define TCG_FEATURE_MULTI_USER  0x0500  /* Multi User support */

/* ====================================================================
 *  TCG Storage Opal SSC Feature Descriptor (Feature Code 0x0001)
 * ==================================================================== */

#define TCG_SSC_OPAL_V1          0x01  /* Opal SSC v1.0 */
#define TCG_SSC_OPAL_V2          0x02  /* Opal SSC v2.01 */
#define TCG_SSC_PYRITE_V1        0x03  /* Pyrite SSC v1.0 */
#define TCG_SSC_ENTERPRISE       0x04  /* Enterprise SSC */
#define TCG_SSC_RUBY_V1          0x05  /* Ruby SSC v1.0 */

/* Supported features byte flags */
#define TCG_SSCF_SYNC            (1u << 0)  /* Sync operation supported */
#define TCG_SSCF_ASYNC           (1u << 1)  /* Async operation supported */
#define TCG_SSCF_ACK             (1u << 2)  /* Acknowledgement supported */
#define TCG_SSCF_BUFFER_MGMT     (1u << 3)  /* Buffer management supported */

/* ====================================================================
 *  TCG ComPacket / SubPacket / Command — Protocol Structure
 * ==================================================================== */

/* ComPacket header (16 bytes, all big-endian) */
#define OPAL_CP_HDR_SIZE         16
#define OPAL_CP_RESERVED_0       0x0000u

/* ComPacket types */
#define OPAL_CP_TYPE_KEEP_ALIVE  0x0000  /* Keep-alive packet */
#define OPAL_CP_TYPE_SESSION     0x0001  /* Session packet */
#define OPAL_CP_TYPE_MGMT        0x0002  /* Management packet */

/* SubPacket header (16 bytes, all big-endian) */
#define OPAL_SP_HDR_SIZE         16
#define OPAL_SP_RESERVED_0       0x0000u

/* SubPacket kinds */
#define OPAL_SP_KIND_METHOD      0x0001  /* Method invocation */
#define OPAL_SP_KIND_SESSION     0x0002  /* Session management */
#define OPAL_SP_KIND_METHOD_STS  0x0003  /* Method status (response) */

/* ====================================================================
 *  TCG Method Status Codes
 * ==================================================================== */

#define OPAL_METHOD_STATUS_SUCCESS      0x00
#define OPAL_METHOD_STATUS_NOT_AUTHORIZED 0x01
#define OPAL_METHOD_STATUS_OBSOLETE     0x02
#define OPAL_METHOD_STATUS_SP_BUSY      0x03
#define OPAL_METHOD_STATUS_SP_FAILED    0x04
#define OPAL_METHOD_STATUS_SP_DISABLED  0x05
#define OPAL_METHOD_STATUS_SP_FROZEN    0x06
#define OPAL_METHOD_STATUS_NO_SESSIONS_AVAIL 0x07
#define OPAL_METHOD_STATUS_UNIQUENESS_CONFLICT 0x08
#define OPAL_METHOD_STATUS_INSUFFICIENT_SPACE 0x09
#define OPAL_METHOD_STATUS_INSUFFICIENT_ROWS 0x0A
#define OPAL_METHOD_STATUS_INVALID_PARAMETER 0x0B
#define OPAL_METHOD_STATUS_INVALID_REFERENCE 0x0C
#define OPAL_METHOD_STATUS_UNKNOWN_ERROR   0x0D
#define OPAL_METHOD_STATUS_TPER_MALFORMED  0x0E

/* ====================================================================
 *  TCG UIDs (Universally Unique Identifiers) — Commonly Used
 * ==================================================================== */

/* Authorities */
#define OPAL_UID_ADMIN_SP         0x00000000, 0x00000001, 0x00000000, 0x00000001
#define OPAL_UID_LOCKING_SP       0x00000000, 0x00000001, 0x00000000, 0x00000002
#define OPAL_UID_ADMIN1           0x00000000, 0x00000009, 0x00000000, 0x00200001
#define OPAL_UID_SID              0x00000000, 0x00000009, 0x00000000, 0x00210101
#define OPAL_UID_PSID             0x00000000, 0x00000009, 0x00000000, 0x00210102
#define OPAL_UID_HALF_AUTH        0x00000000, 0x00000009, 0x00000000, 0x00210103
#define OPAL_UID_ANYBODY          0x00000000, 0x00000009, 0x00000000, 0x00210107

/* Tables */
#define OPAL_UID_GLOBAL_RANGE     0x00000000, 0x00000008, 0x00000000, 0x00000001
#define OPAL_UID_LOCKING_TABLE    0x00000000, 0x00000008, 0x00000000, 0x00000003
#define OPAL_UID_MBR_TABLE        0x00000000, 0x00000008, 0x00000000, 0x00000004
#define OPAL_UID_ACE_TABLE        0x00000000, 0x00000008, 0x00000000, 0x00000005
#define OPAL_UID_C_PIN_TABLE      0x00000000, 0x00000008, 0x00000000, 0x0000000B
#define OPAL_UID_STORE_TABLE      0x00000000, 0x00000008, 0x00000000, 0x0000000D

/* ====================================================================
 *  Locking Range Identifiers (LBA ranges)
 * ==================================================================== */

#define OPAL_LOCKING_RANGE_GLOBAL 0   /* Global range (entire disk) */
#define OPAL_LOCKING_RANGE_1      1   /* Range 1 */
#define OPAL_LOCKING_RANGE_2      2   /* Range 2 */
#define OPAL_LOCKING_RANGE_3      3   /* Range 3 */
#define OPAL_LOCKING_RANGE_4      4   /* Range 4 */
#define OPAL_LOCKING_RANGE_5      5   /* Range 5 */
#define OPAL_LOCKING_RANGE_6      6   /* Range 6 */
#define OPAL_LOCKING_RANGE_7      7   /* Range 7 */
#define OPAL_LOCKING_RANGE_8      8   /* Range 8 */
#define OPAL_LOCKING_RANGE_MAX    8

/* ====================================================================
 *  Locking Range States
 * ==================================================================== */

#define OPAL_LOCKING_READ_LOCKED   (1u << 0)
#define OPAL_LOCKING_WRITE_LOCKED  (1u << 1)
#define OPAL_LOCKING_RW_LOCKED     (OPAL_LOCKING_READ_LOCKED | \
                                     OPAL_LOCKING_WRITE_LOCKED)

/* ====================================================================
 *  Opal Method Tokens (Tiny/Short/Medium atoms)
 * ==================================================================== */

/* Tiny atom: 0x00-0x7F = value 0-127 */
#define OPAL_TINY_ATOM(val)       ((uint8_t)(val) & 0x7F)

/* Short atom: 0x80-0xBF, length follows (0-63 bytes) */
#define OPAL_SHORT_ATOM_HDR(len)  ((uint8_t)(0x80 | ((len) & 0x3F)))
#define OPAL_IS_TINY(b)           (!((b) & 0x80))
#define OPAL_IS_SHORT(b)          (((b) & 0xC0) == 0x80)
#define OPAL_IS_MEDIUM(b)         (((b) & 0xC0) == 0xC0)
#define OPAL_ATOM_LEN(b)          ((b) & 0x3F)

/* ====================================================================
 *  Opal Method Start / End Tokens
 * ==================================================================== */

#define OPAL_TOKEN_METHOD_START    0xF0
#define OPAL_TOKEN_METHOD_END      0xF1
#define OPAL_TOKEN_METHOD_LIST     0xF2
#define OPAL_TOKEN_CLOSE_NAME     0xF3
#define OPAL_TOKEN_CLOSE_BRACE    0xF4
#define OPAL_TOKEN_CLOSE_LIST     0xF5
#define OPAL_TOKEN_START_LIST     0xF6
#define OPAL_TOKEN_OPEN_NAME      0xF7
#define OPAL_TOKEN_OPEN_BRACE     0xF8
#define OPAL_TOKEN_OPEN_LIST      0xF9
#define OPAL_TOKEN_END_OF_DATA    0xFA
#define OPAL_TOKEN_START_TRANS    0xFB
#define OPAL_TOKEN_END_TRANS      0xFC
#define OPAL_TOKEN_EMPTY_BRACE    0xFD
#define OPAL_TOKEN_EMPTY_LIST     0xFE

/* ====================================================================
 *  Standard Opal Methods (invoked by name)
 * ==================================================================== */

#define OPAL_METHOD_REVERT          "revert"
#define OPAL_METHOD_REVERTSP        "revertSP"
#define OPAL_METHOD_ACTIVATE        "activate"
#define OPAL_METHOD_ACTIVATE_USING  "activateUsing"
#define OPAL_METHOD_GEN_KEY         "genKey"
#define OPAL_METHOD_NEXT            "next"
#define OPAL_METHOD_GET             "get"
#define OPAL_METHOD_SET             "set"
#define OPAL_METHOD_PROPERTIES      "properties"
#define OPAL_METHOD_START_SESSION   "startSession"
#define OPAL_METHOD_AUTHENTICATE    "authenticate"
#define OPAL_METHOD_LOCK_UNLOCK     "lockUnlock"
#define OPAL_METHOD_MBR             "mbr"
#define OPAL_METHOD_ERASE           "erase"

/* ====================================================================
 *  Session flags
 * ==================================================================== */

#define OPAL_SESSION_FLAG_RW       0x01
#define OPAL_SESSION_FLAG_READONLY 0x02
#define OPAL_SESSION_FLAG_MGMT     0x04

/* ====================================================================
 *  TCG Opal Discovery data structures (Level 0)
 * ==================================================================== */

/** TCG Storage feature descriptor — parsed from Level 0 Discovery */
struct tcg_feature_desc {
    uint16_t feature_code;
    uint8_t  version;
    uint8_t  reserved;
    uint16_t length;
    const uint8_t *data;  /* Pointer into the discovery buffer */
};

/** Parsed Level 0 Discovery result */
struct opal_discovery0 {
    int           valid;           /* Non-zero if discovery data parsed */
    uint32_t      data_length;     /* Total length of feature data */

    /* Feature presence flags */
    int           has_tcg_storage; /* TCG Storage feature present */
    int           has_opal_v1;     /* Opal SSC v1.0 */
    int           has_opal_v2;     /* Opal SSC v2.01 */
    int           has_pyrite;      /* Pyrite SSC */
    int           has_enterprise;  /* Enterprise SSC */

    /* TCG Storage details */
    uint8_t       ssc;             /* SSC type (TCG_SSC_OPAL_V1, etc.) */
    uint8_t       ssc_version;     /* Version of the SSC feature descriptor */
    uint8_t       ssc_supported_features; /* Supported features byte */

    /* Locking info from discovery */
    int           num_locking_ranges;  /* Number of supported locking ranges */
    int           locking_enabled;     /* Locking SP is enabled */
    int           locking_locked;      /* Drive is currently locked */

    /* Geometry */
    uint64_t      alignment_granularity;
    uint64_t      lowest_lba;
};

/* ====================================================================
 *  Opal session handle
 * ==================================================================== */

struct opal_session {
    int       active;             /* Non-zero if session is active */
    int       bus;                /* ATA bus number */
    int       master;             /* Drive select (0=master, 1=slave) */
    uint32_t  sp_uid[4];          /* SP UID (usually Locking SP) */
    int       read_only;          /* Session is read-only */
    int       host_session_num;   /* Host session number */
    uint32_t  tper_session_num;   /* TPer session number */
    uint32_t  seq_num;            /* Sequence number for commands */
    uint8_t   key[32];            /* Session key (if authenticated) */
    int       key_len;            /* Key length in bytes */
};

/* ====================================================================
 *  Public API — ATA-Level Trusted Send/Receive
 * ==================================================================== */

/**
 * ata_opal_trusted_send — Send data via ATA TRUSTED SEND (0x5B).
 * @bus:      ATA bus number (0=primary, 1=secondary)
 * @master:   Drive select (0=master, 1=slave)
 * @protocol: Security protocol (e.g. TCG_SP_TCG_STORAGE)
 * @sp_specific: Protocol-specific value (SP specific field)
 * @data:     Data buffer to send (must be sector-aligned)
 * @nbytes:   Number of bytes to send (must be multiple of 512)
 *
 * Returns 0 on success, negative errno on failure.
 */
int ata_opal_trusted_send(int bus, int master, uint8_t protocol,
                          uint32_t sp_specific, const void *data,
                          size_t nbytes);

/**
 * ata_opal_trusted_recv — Receive data via ATA TRUSTED RECEIVE (0x5C).
 * @bus:      ATA bus number
 * @master:   Drive select
 * @protocol: Security protocol
 * @sp_specific: Protocol-specific value
 * @data:     Buffer to receive into (must be sector-aligned)
 * @nbytes:   Number of bytes to receive (must be multiple of 512)
 *
 * Returns 0 on success, negative errno on failure.
 */
int ata_opal_trusted_recv(int bus, int master, uint8_t protocol,
                          uint32_t sp_specific, void *data,
                          size_t nbytes);

/* ====================================================================
 *  Public API — Level 0 Discovery
 * ==================================================================== */

/**
 * ata_opal_discovery0 — Perform TCG Storage Level 0 Discovery.
 * @bus:      ATA bus number
 * @master:   Drive select
 * @info:     Output structure for parsed discovery data
 *
 * Allocates a temporary buffer, performs TRUSTED RECEIVE with
 * TCG_SP_TCG_STORAGE, parses the feature descriptors, and populates
 * the opal_discovery0 structure.
 *
 * Returns 0 on success, negative errno on failure.
 */
int ata_opal_discovery0(int bus, int master,
                        struct opal_discovery0 *info);

/**
 * ata_opal_is_supported — Quick check if drive supports TCG Opal.
 * @bus:      ATA bus number
 * @master:   Drive select
 *
 * Performs Level 0 Discovery and checks for Opal SSC support.
 * Returns 1 if Opal is supported, 0 if not, negative errno on I/O error.
 */
int ata_opal_is_supported(int bus, int master);

/* ====================================================================
 *  Public API — Session Management
 * ==================================================================== */

/**
 * ata_opal_start_session — Start a TCG Storage session.
 * @session:  Session handle (output)
 * @bus:      ATA bus number
 * @master:   Drive select
 * @sp_uid:   SP UID (4 x uint32_t) to open session on
 * @host_challenge: Optional challenge data for authentication (may be NULL)
 * @challenge_len:  Length of challenge data
 *
 * Returns 0 on success (session->active = 1), negative errno on failure.
 */
int ata_opal_start_session(struct opal_session *session, int bus,
                           int master, const uint32_t *sp_uid,
                           const uint8_t *host_challenge,
                           int challenge_len);

/**
 * ata_opal_end_session — End an active TCG Storage session.
 * @session:  Active session handle
 *
 * Returns 0 on success, negative errno on failure.
 */
int ata_opal_end_session(struct opal_session *session);

/* ====================================================================
 *  Public API — Locking Operations
 * ==================================================================== */

/**
 * ata_opal_set_lock_state — Lock or unlock an Opal locking range.
 * @session:  An active, authenticated session (on Locking SP)
 * @range:    Locking range number (OPAL_LOCKING_RANGE_GLOBAL, etc.)
 * @read_lock:   Non-zero to set read-locked
 * @write_lock:  Non-zero to set write-locked
 *
 * The session must be authenticated as Admin1 or SID on the Locking SP.
 *
 * Returns 0 on success, negative errno on failure.
 */
int ata_opal_set_lock_state(struct opal_session *session, int range,
                            int read_lock, int write_lock);

/**
 * ata_opal_revert_tper — Revert TPer to factory defaults (using SID).
 * @session:  An active, authenticated session (on Admin SP as SID)
 *
 * WARNING: This erases all data on the drive!
 *
 * Returns 0 on success, negative errno on failure.
 */
int ata_opal_revert_tper(struct opal_session *session);

#endif /* ATA_OPAL_H */
