#ifndef LUKS_H
#define LUKS_H

#include "types.h"
#include "blockdev.h"

/*
 * LUKS (Linux Unified Key Setup) — B18
 *
 * On-disk format (LUKS v1, first 512-byte sector):
 *
 *   Offset  Size  Field
 *   ─────────────────────
 *   0       6     magic        "LUKS\xBA\xBE"
 *   6       2     version      1 (big-endian uint16_t)
 *   8       32    cipher_name  e.g. "aes"
 *   40      32    cipher_mode  e.g. "xts-plain64"
 *   72      32    hash_spec    e.g. "sha256"
 *   104     4     payload_offset  sectors (big-endian uint32_t)
 *   108     4     key_bytes    size of master key (big-endian uint32_t)
 *   112     32    mk_digest    SHA-256 digest of master key
 *   144     32    mk_digest_salt
 *   176     4     mk_digest_iter  (big-endian uint32_t)
 *   180     40    uuid
 *   220     6     padding
 *   226     6     (padding to align key slots)
 *   232     48*8  key_slots[8]  (8 slots, 48 bytes each)
 *
 * Each key slot (48 bytes):
 *   Offset  Size  Field
 *   0       4     state       0x0000=active, 0xDEAD=inactive (big-endian)
 *   4       4     iterations  (big-endian uint32_t)
 *   8       32    salt
 *   40      4     key_material_offset  sectors from start (big-endian)
 *   44      4     stripes     (big-endian uint32_t)
 */

/* LUKS constants */
#define LUKS_MAGIC           "LUKS\xba\xbe"
#define LUKS_MAGIC_LEN       6
#define LUKS_CIPHER_NAME_LEN 32
#define LUKS_CIPHER_MODE_LEN 32
#define LUKS_HASH_SPEC_LEN   32
#define LUKS_UUID_LEN        40
#define LUKS_KEY_SLOTS       8
#define LUKS_SLOT_ACTIVE     0x0000
#define LUKS_SLOT_INACTIVE   0xDEAD
#define LUKS_DIGEST_SIZE     32   /* SHA-256 */

/* Key slot descriptor */
struct luks_keyslot {
    uint32_t state;                   /* active / inactive */
    uint32_t iterations;              /* PBKDF2 iterations */
    uint8_t  salt[32];                /* PBKDF2 salt */
    uint32_t key_material_offset;     /* sector offset of key material */
    uint32_t stripes;                 /* AF split stripes */
};

/* Parsed LUKS header */
struct luks_header {
    uint16_t version;                  /* LUKS version (1) */
    char     cipher_name[LUKS_CIPHER_NAME_LEN];
    char     cipher_mode[LUKS_CIPHER_MODE_LEN];
    char     hash_spec[LUKS_HASH_SPEC_LEN];
    uint32_t payload_offset;           /* data area start (sectors) */
    uint32_t key_bytes;                /* master key size in bytes */
    uint8_t  mk_digest[LUKS_DIGEST_SIZE];    /* master key digest */
    uint8_t  mk_digest_salt[32];             /* digest salt */
    uint32_t mk_digest_iter;                  /* digest PBKDF2 iterations */
    char     uuid[LUKS_UUID_LEN];            /* volume UUID */
    struct luks_keyslot key_slots[LUKS_KEY_SLOTS];
};

/* On-disk LUKS header (raw 512-byte sector) */
struct luks_disk_header {
    uint8_t  magic[6];
    uint8_t  version[2];       /* big-endian */
    uint8_t  cipher_name[LUKS_CIPHER_NAME_LEN];
    uint8_t  cipher_mode[LUKS_CIPHER_MODE_LEN];
    uint8_t  hash_spec[LUKS_HASH_SPEC_LEN];
    uint8_t  payload_offset[4];  /* big-endian */
    uint8_t  key_bytes[4];       /* big-endian */
    uint8_t  mk_digest[20];      /* standard LUKS uses SHA-1 (20 bytes) */
    uint8_t  mk_digest_salt[32];
    uint8_t  mk_digest_iter[4];  /* big-endian */
    uint8_t  uuid[LUKS_UUID_LEN];
    uint8_t  _pad1[6];
    uint8_t  key_slots_raw[LUKS_KEY_SLOTS * 48];
} __attribute__((packed));

/* On-disk key slot (48 bytes) */
struct luks_disk_keyslot {
    uint8_t state[2];         /* big-endian uint16_t */
    uint8_t iterations[4];    /* big-endian uint32_t */
    uint8_t salt[32];
    uint8_t key_material_offset[4];  /* big-endian uint32_t */
    uint8_t stripes[4];       /* big-endian uint32_t */
} __attribute__((packed));

/* ── Public API ──────────────────────────────────────────────────── */

/**
 * luks_parse_header - Read and parse LUKS header from a block device.
 * @dev_id:  Block device ID
 * @hdr:     Output: parsed LUKS header structure
 *
 * Returns 0 on success, negative errno on failure.
 */
int luks_parse_header(int dev_id, struct luks_header *hdr);

/**
 * luks_open_keyslot - Derive master key from passphrase via PBKDF2.
 * @dev_id:     Block device ID
 * @hdr:        Parsed LUKS header
 * @slot:       Key slot index (0-7)
 * @passphrase: User-supplied passphrase
 * @mk:         Output: master key (must have hdr->key_bytes space)
 *
 * Returns 0 on success, negative errno on failure.
 *   -EPERM  if passphrase is wrong / digest doesn't match
 *   -ENOENT if key slot is inactive
 */
int luks_open_keyslot(int dev_id, struct luks_header *hdr, int slot,
                      const char *passphrase, uint8_t *mk);

/**
 * luks_setup_dm_crypt - Create a dm-crypt mapping using LUKS parameters.
 * @dev_id:     Block device ID
 * @hdr:        Parsed LUKS header
 * @mk:         Master key (key_bytes bytes)
 *
 * Creates a dm-crypt target that covers from payload_offset to end of device.
 * Returns the dm device ID on success, negative errno on failure.
 */
int luks_setup_dm_crypt(int dev_id, struct luks_header *hdr, const uint8_t *mk);

#endif /* LUKS_H */
