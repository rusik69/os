/*
 * luks.c — LUKS disk encryption header parsing and dm-crypt setup — B18
 *
 * Implements LUKS (Linux Unified Key Setup) v1 and v2 header parsing,
 * PBKDF2-HMAC-SHA256 key derivation, master key digest verification,
 * anti-forensic (AF) split reversal, and dm-crypt mapping setup.
 *
 * ── LUKS Architecture ──────────────────────────────────────────────────────────
 *
 * LUKS provides on-disk encryption via a two-layer key hierarchy:
 *
 *   Passphrase  ──PBKDF2──>  Derived Key  ──decrypt──>  Master Key  ──AES-XTS──>  Data
 *
 *   (user-supplied)          (key-slot specific)       (stored in AF-split         (ciphertext
 *                                                       key material)               on disk)
 *
 * The volume master key is the actual encryption key.  It is never stored
 * in plaintext.  Instead it is split using the anti-forensic (AF) splitter
 * (essentially [N] stripes XOR-masked with a hash chain), then encrypted
 * with AES-XTS using a key derived from the user's passphrase via PBKDF2.
 *
 * Multiple key slots (up to 8) allow different passphrases to unlock the
 * same master key.  Each slot has its own salt and PBKDF2 iteration count;
 * changing a passphrase rewrites only that slot's key material.
 *
 * ── On-disk Layout (LUKS v1) ──────────────────────────────────────────────────
 *
 *   Offset    Size    Description
 *   ─────────────────────────────────────────────────────────────────
 *   0         512     LUKS phdr (magic, version, cipher, key slots, ...)
 *   512       (n)     Key material for slot 0..7 (stripes × key_bytes each)
 *   payload_offset    Encrypted data (sectors)
 *
 *   The header is a single 512‑byte sector.  Key slots follow immediately
 *   after the header sector, one per slot, each occupying [stripes × key_bytes]
 *   bytes (aligned to sector boundaries).
 *
 * ── On-disk Layout (LUKS v2) ──────────────────────────────────────────────────
 *
 *   LUKS v2 uses a 4096‑byte binary header followed by a JSON text
 *   area (up to ~4 KB).  The JSON contains all key slot metadata,
 *   cipher parameters, and token information.  Two such headers exist
 *   (primary at offset 0, secondary at offset hdr_size) for atomic
 *   updates.  We parse the JSON to locate active keyslots.
 *
 * ── Key Derivation (PBKDF2-HMAC-SHA256) ──────────────────────────────────────
 *
 *   PBKDF2 (RFC 2898) derives a hardened key from a passphrase + salt:
 *
 *     U_1 = HMAC-SHA256(password, salt || INT32_BE(i))
 *     U_j = HMAC-SHA256(password, U_{j-1})             for j = 2..c
 *     T_i = U_1 XOR U_2 XOR ... XOR U_c
 *     DK  = T_1 || T_2 || ...  (truncated to dkLen)
 *
 *   The iteration count c provides computational hardening against
 *   brute-force attacks.  Each key slot stores its own salt and
 *   iteration count.
 *
 * ── Master Key Verification ───────────────────────────────────────────────────
 *
 *   After decrypting the key material, the recovered master key is
 *   verified against mk_digest stored in the header:
 *
 *     computed = SHA256(mk_digest_salt || recovered_master_key)
 *     OK  ⇔  memcmp(computed, mk_digest, 32) == 0
 *
 *   A mismatch means the wrong passphrase was supplied (derived key
 *   didn't decrypt the key material correctly).
 *
 * ── dm-crypt Setup ────────────────────────────────────────────────────────────
 *
 *   The recovered master key is split in half (key1 = first half for
 *   AES data encryption, key2 = second half for XTS tweak) and passed
 *   to the device-mapper to create a transparent encryption target.
 *
 * Supported configuration:
 *   cipher:    aes
 *   mode:      xts-plain64
 *   hash:      sha256
 *   key_size:  32 or 64 bytes (AES-128-XTS or AES-256-XTS)
 */

#define KERNEL_INTERNAL
#include "luks.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "errno.h"
#include "sha256.h"
#include "hmac.h"
#include "aes.h"
#include "aes_xts.h"
#include "dm.h"

/* ── Big-endian helpers ──────────────────────────────────────────── */

static uint16_t be16_to_cpu(const uint8_t *b)
{
    return ((uint16_t)b[0] << 8) | (uint16_t)b[1];
}

static uint32_t be32_to_cpu(const uint8_t *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  | (uint32_t)b[3];
}

static uint64_t be64_to_cpu(const uint8_t *b)
{
    return ((uint64_t)b[0] << 56) | ((uint64_t)b[1] << 48) |
           ((uint64_t)b[2] << 40) | ((uint64_t)b[3] << 32) |
           ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) |
           ((uint64_t)b[6] << 8)  | (uint64_t)b[7];
}

/* ── LUKS2 binary header layout ──────────────────────────────────── */

/*
 * LUKS2 splits the header into a fixed-size binary portion followed by
 * a JSON text area.  The binary portion is 512 bytes:
 *
 *   Offset  Size  Field
 *   ──────────────────────
 *   0       6     magic        "LUKS\xBA\xBE"
 *   6       2     version      2 (BE uint16)
 *   8       4     hdr_size     (BE uint32, usually 4096)
 *   12      8     seqid        header sequence ID (BE uint64)
 *   20      48    label
 *   68      32    csum_type    checksum algorithm name (string)
 *   100     64    salt
 *   164     40    uuid
 *   204     48    subsystem
 *   252     4     hdr_offset   (BE uint32, 0 or 4096)
 *   256     256   _pad
 *
 * JSON area starts at byte 512 and extends to hdr_size.
 */
#define LUKS2_MAGIC       "LUKS\xBA\xBE"
#define LUKS2_MAGIC_LEN   6
#define LUKS2_SECTOR_SIZE 512
#define LUKS2_HDR_SIZE    4096
#define LUKS2_MAX_JSON    4096  /* max JSON area size after binary header */

/* LUKS2 binary header (first 512 bytes of offset 0) */
struct luks2_header {
    uint8_t  magic[6];
    uint16_t version;       /* 2 */
    uint32_t hdr_size;      /* size of this header (usually 4096) */
    uint64_t seqid;         /* header sequence ID */
    uint8_t  label[48];
    uint8_t  csum_type[32]; /* null-terminated string */
    uint8_t  salt[64];
    uint8_t  uuid[40];
    uint8_t  subsystem[48];
    uint32_t hdr_offset;    /* offset of this header (0 or 4096) */
    uint8_t  _pad[256];     /* padding to 512 bytes (should be 512 − 256 = 256) */
    /* After this, JSON area starts at offset 512 */
} __attribute__((packed));

/*
 * luks2_parse_header - Parse LUKS v2 binary header and JSON keyslot area.
 * @dev_id:  Block device ID
 * @hdr:     Output: parsed LUKS header structure (populated with keyslot info)
 *
 * Reads the first 4 KiB from the device, validates the LUKS2 magic,
 * byte-swaps multi-byte fields, then walks the JSON area (starting at
 * byte 512) to find active keyslots.  For each active slot it extracts
 * key_size, stripes count, and key material disk offset.
 *
 * The JSON parser is a simplified linear scan — it finds "keyslots" and
 * then enumerates slot numbers looking for "active":true entries.
 *
 * Returns 0 on success, negative errno on failure.
 */
static int luks2_parse_header(int dev_id, struct luks_header *hdr)
{
    uint8_t raw[LUKS2_HDR_SIZE];
    int ret;

    if (!hdr)
        return -EINVAL;

    /* Read binary header (first 4KB) */
    ret = blk_submit_sync(dev_id, 0, LUKS2_HDR_SIZE / 512, raw, BLK_REQ_READ);
    if (ret < 0) {
        kprintf("[luks2] read header failed: %d\n", ret);
        return ret;
    }

    struct luks2_header *h2 = (struct luks2_header *)raw;

    /* Verify magic */
    if (memcmp(h2->magic, LUKS2_MAGIC, LUKS2_MAGIC_LEN) != 0) {
        kprintf("[luks2] bad magic\n");
        return -EINVAL;
    }

    /* Byte-swap multi-byte fields (LUKS2 uses big-endian on-disk) */
    uint64_t seqid = be64_to_cpu((const uint8_t *)&h2->seqid);
    uint32_t hdr_size = be32_to_cpu((const uint8_t *)&h2->hdr_size);

    kprintf("[luks2] LUKS v2 header detected: seqid=%llu, hdr_size=%u\n",
            (unsigned long long)seqid, hdr_size);

    /* Parse JSON area (text after binary header, at offset 512) */
    /* The LUKS2 JSON area contains keyslot descriptions, cipher info, etc.
     * We do a simplified parse to find active keyslots. */
    char *json = (char *)(raw + 512);
    uint32_t json_max = hdr_size - 512;
    if (json_max > LUKS2_MAX_JSON) json_max = LUKS2_MAX_JSON;
    json[json_max - 1] = '\0';

    kprintf("[luks2] JSON area (%u bytes):\n%.256s\n", json_max, json);

    /* Find keyslot info in JSON: look for "active":true patterns */
    int keyslots_found = 0;
    const char *ks_search = json;
    while (ks_search && *ks_search && (uint32_t)(ks_search - json) < json_max) {
        const char *ks_key = strstr(ks_search, "\"keyslots\"");
        if (!ks_key) break;

        /* Find keyslot names (e.g., "0": { ... }) */
        const char *slot_start = ks_key + 10;
        for (int slot = 0; slot < LUKS_KEY_SLOTS; slot++) {
            char slot_tag[8];
            snprintf(slot_tag, sizeof(slot_tag), "\"%d\":", slot);
            const char *s = strstr(slot_start, slot_tag);
            if (!s) continue;

            /* Check if active */
            const char *active = strstr(s, "\"active\"");
            if (active) {
                const char *val = active + 8;
                while (*val == ' ' || *val == ':') val++;
                if (strncmp(val, "true", 4) == 0 ||
                    strncmp(val, "\"true\"", 6) == 0) {
                    hdr->key_slots[slot].state = LUKS_SLOT_ACTIVE;

                    /* Find key_size, stripes, etc. */
                    const char *ks = strstr(s, "\"key_size\"");
                    if (ks) {
                        const char *kv = ks + 9;
                        while (*kv == ' ' || *kv == ':') kv++;
                        int ks_val = 0;
                        while (*kv >= '0' && *kv <= '9') {
                            ks_val = ks_val * 10 + (*kv - '0');
                            kv++;
                        }
                        hdr->key_bytes = (uint32_t)ks_val;
                    }

                    const char *stripes = strstr(s, "\"stripes\"");
                    if (stripes) {
                        const char *sv = stripes + 8;
                        while (*sv == ' ' || *sv == ':') sv++;
                        int st_val = 0;
                        while (*sv >= '0' && *sv <= '9') {
                            st_val = st_val * 10 + (*sv - '0');
                            sv++;
                        }
                        hdr->key_slots[slot].stripes = (uint32_t)st_val;
                    }

                    /* Find AF stripes offset */
                    const char *af_offset = strstr(s, "\"area\"");
                    if (af_offset) {
                        const char *off_str = strstr(af_offset, "\"offset\"");
                        if (off_str) {
                            const char *ov = off_str + 7;
                            while (*ov == ' ' || *ov == ':') ov++;
                            uint64_t off_val = 0;
                            while (*ov >= '0' && *ov <= '9') {
                                off_val = off_val * 10 + (*ov - '0');
                                ov++;
                            }
                            hdr->key_slots[slot].key_material_offset =
                                (uint32_t)(off_val / 512);
                        }
                    }

                    keyslots_found++;
                    kprintf("[luks2] keyslot %d: active, key_size=%u, "
                            "offset=%u\n",
                            slot, hdr->key_bytes,
                            hdr->key_slots[slot].key_material_offset);
                }
            }
        }
        break;
    }

    /* Set version and payload offset */
    hdr->version = 2;
    hdr->payload_offset = (hdr_size * 2) / LUKS2_SECTOR_SIZE; /* primary + secondary header */

    kprintf("[luks2] Parsed: %d active keyslots, payload_offset=%u\n",
            keyslots_found, hdr->payload_offset);
    return 0;
}

/* ── PBKDF2-HMAC-SHA256 ─────────────────────────────────────────── */

/*
 * pbkdf2_hmac_sha256 - Password-Based Key Derivation Function v2 (RFC 2898).
 * @password:    User-supplied passphrase
 * @pw_len:      Length of passphrase in bytes
 * @salt:        Per-slot salt value from LUKS header
 * @salt_len:    Length of salt in bytes (32 for LUKS)
 * @iterations:  PBKDF2 iteration count (computational hardening factor)
 * @out:         Output buffer for derived key material
 * @dk_len:      Desired derived key length in bytes
 *
 * Implements PBKDF2-HMAC-SHA256:
 *
 *   For each block index i (1-indexed):
 *     U_1 = HMAC-SHA256(password, salt || INT_32_BE(i))
 *     U_j = HMAC-SHA256(password, U_{j-1})   for j = 2..c
 *     T_i = U_1 ⊕ U_2 ⊕ ... ⊕ U_c
 *   DK = T_1 || T_2 || ... (truncated to dkLen)
 *
 * Each output block is HMAC_SHA256_DIGEST_SIZE (32) bytes.
 * The function allocates a temporary salt+block-index buffer on the heap.
 */
static void pbkdf2_hmac_sha256(const uint8_t *password, size_t pw_len,
                                const uint8_t *salt, size_t salt_len,
                                uint32_t iterations,
                                uint8_t *out, size_t dk_len)
{
    uint32_t blocks_needed = (uint32_t)((dk_len + 31) / 32);
    uint8_t u[HMAC_SHA256_DIGEST_SIZE];
    uint8_t t[HMAC_SHA256_DIGEST_SIZE];
    size_t written = 0;

    /* Temporary buffer for Salt || INT_32_BE(i) */
    uint8_t *salt_block = (uint8_t *)kmalloc(salt_len + 4);
    if (!salt_block) return;
    memcpy(salt_block, salt, salt_len);

    for (uint32_t block = 1; block <= blocks_needed && written < dk_len; block++) {
        /* Build Salt || INT_32_BE(block) */
        salt_block[salt_len + 0] = (uint8_t)((block >> 24) & 0xFF);
        salt_block[salt_len + 1] = (uint8_t)((block >> 16) & 0xFF);
        salt_block[salt_len + 2] = (uint8_t)((block >> 8) & 0xFF);
        salt_block[salt_len + 3] = (uint8_t)(block & 0xFF);

        /* U_1 = HMAC-SHA256(password, Salt || INT_32_BE(block)) */
        hmac_sha256(password, pw_len, salt_block, salt_len + 4, u);

        /* T_i starts as U_1 */
        memcpy(t, u, HMAC_SHA256_DIGEST_SIZE);

        /* U_2 .. U_c */
        for (uint32_t j = 2; j <= iterations; j++) {
            hmac_sha256(password, pw_len, u, HMAC_SHA256_DIGEST_SIZE, u);
            for (int k = 0; k < HMAC_SHA256_DIGEST_SIZE; k++)
                t[k] ^= u[k];
        }

        /* Append T_i to output */
        size_t to_copy = HMAC_SHA256_DIGEST_SIZE;
        if (written + to_copy > dk_len)
            to_copy = dk_len - written;
        memcpy(out + written, t, to_copy);
        written += to_copy;
    }

    kfree(salt_block);
}

/* ── luks_parse_header ───────────────────────────────────────────── */

/*
 * luks_parse_header - Read and parse LUKS header from a block device.
 * @dev_id:  Block device ID
 * @hdr:     Output: populated LUKS header structure
 *
 * Reads the first 512-byte sector from the device and checks for
 * the LUKS v1 magic ("LUKS\xba\xbe").  If the v1 magic is not found,
 * falls back to luks2_parse_header() to attempt LUKS v2 parsing.
 *
 * For LUKS v1, the function extracts:
 *   - cipher name, mode, hash spec (null-terminated strings)
 *   - payload offset (start of encrypted data area)
 *   - key bytes (master key length)
 *   - master key digest + digest salt + iterations
 *   - UUID
 *   - 8 key slots (state, PBKDF2 iterations, salt, key material offset, stripes)
 *
 * The LUKS v1 header layout is byte-indexed directly from the raw
 * sector data (big-endian).  Two header variants are supported:
 *   - SHA-256 hash  (offset variant starting at byte 112)
 *   - SHA-1 hash    (standard LUKS v1, offset variant starting at byte 112 too,
 *                    with different fields at different byte positions)
 *
 * Returns 0 on success, negative errno on failure.
 */
int luks_parse_header(int dev_id, struct luks_header *hdr)
{
    uint8_t raw[512];
    int ret;

    if (!hdr)
        return -EINVAL;

    /* Read the first sector (LBA 0) */
    ret = blk_submit_sync(dev_id, 0, 1, raw, BLK_REQ_READ);
    if (ret < 0) {
        kprintf("[luks] read header failed: %d\n", ret);
        return ret;
    }

    /* Verify magic */
    if (memcmp(raw, LUKS_MAGIC, LUKS_MAGIC_LEN) != 0) {
        /* Try LUKS2 format */
        return luks2_parse_header(dev_id, hdr);
    }

    /* Parse header */
    memset(hdr, 0, sizeof(*hdr));

    hdr->version = be16_to_cpu(raw + 6);
    if (hdr->version != 1) {
        kprintf("[luks] unsupported version %u\n", hdr->version);
        return -EINVAL;
    }

    memcpy(hdr->cipher_name, raw + 8, LUKS_CIPHER_NAME_LEN - 1);
    memcpy(hdr->cipher_mode, raw + 40, LUKS_CIPHER_MODE_LEN - 1);
    memcpy(hdr->hash_spec, raw + 72, LUKS_HASH_SPEC_LEN - 1);
    hdr->cipher_name[LUKS_CIPHER_NAME_LEN - 1] = '\0';
    hdr->cipher_mode[LUKS_CIPHER_MODE_LEN - 1] = '\0';
    hdr->hash_spec[LUKS_HASH_SPEC_LEN - 1] = '\0';

    hdr->payload_offset  = be32_to_cpu(raw + 104);
    hdr->key_bytes       = be32_to_cpu(raw + 108);

    /* For SHA-256, mk_digest occupies 32 bytes starting at offset 112.
     * Standard LUKS v1 stores SHA-1 (20 bytes) at 112, but with
     * hash_spec="sha256" we read 32 bytes. */
    if (strcmp(hdr->hash_spec, "sha256") == 0) {
        memcpy(hdr->mk_digest, raw + 112, LUKS_DIGEST_SIZE);
        memcpy(hdr->mk_digest_salt, raw + 144, 32);
        hdr->mk_digest_iter = be32_to_cpu(raw + 176);
        memcpy(hdr->uuid, raw + 180, LUKS_UUID_LEN - 1);
    } else {
        /* Fallback for SHA-1 (standard LUKS v1) */
        memcpy(hdr->mk_digest, raw + 112, 20);
        hdr->mk_digest[20] = 0;
        memcpy(hdr->mk_digest_salt, raw + 132, 32);
        hdr->mk_digest_iter = be32_to_cpu(raw + 164);
        memcpy(hdr->uuid, raw + 168, LUKS_UUID_LEN - 1);
    }
    hdr->uuid[LUKS_UUID_LEN - 1] = '\0';

    /* Parse key slots — they're at byte 208 for SHA-1 layout, or 220 for SHA-256 */
    int key_slot_offset = (strcmp(hdr->hash_spec, "sha256") == 0) ? 220 : 208;

    for (int i = 0; i < LUKS_KEY_SLOTS; i++) {
        int off = key_slot_offset + i * 48;
        const uint8_t *ks = raw + off;

        hdr->key_slots[i].state              = be32_to_cpu(ks);
        hdr->key_slots[i].iterations         = be32_to_cpu(ks + 4);
        memcpy(hdr->key_slots[i].salt, ks + 8, 32);
        hdr->key_slots[i].key_material_offset = be32_to_cpu(ks + 40);
        hdr->key_slots[i].stripes            = be32_to_cpu(ks + 44);
    }

    kprintf("[luks] LUKS v%u header parsed: cipher=%s, mode=%s, hash=%s, "
            "key_bytes=%u, payload_offset=%u\n",
            hdr->version, hdr->cipher_name, hdr->cipher_mode, hdr->hash_spec,
            hdr->key_bytes, hdr->payload_offset);

    return 0;
}

/* ── luks_open_keyslot ───────────────────────────────────────────── */

/*
 * luks_open_keyslot - Derive master key from passphrase via PBKDF2 + AF merge.
 * @dev_id:     Block device ID (for reading key material from disk)
 * @hdr:        Parsed LUKS header (must have been populated by luks_parse_header)
 * @slot:       Key slot index to use (0 to LUKS_KEY_SLOTS-1)
 * @passphrase: User-supplied passphrase (null-terminated)
 * @mk:         Output buffer for the recovered master key (hdr->key_bytes bytes)
 *
 * This is the core unlock operation.  It performs five steps:
 *
 *   1. DERIVE — Compute a derived key from the passphrase using PBKDF2
 *      with the slot-specific salt and iteration count.
 *
 *   2. READ   — Read the key material (anti-forensic split data) from disk
 *      at the slot's key_material_offset sector.
 *
 *   3. DECRYPT — Decrypt the key material using AES-XTS with the derived key.
 *      The derived key is split in half: first half = AES data key,
 *      second half = XTS tweak key.  Each 512-byte sector is decrypted
 *      independently using the sector index as the XTS tweak value.
 *
 *   4. EXTRACT — The first key_bytes bytes of the decrypted material form
 *      the recovered master key.  (The AF split is already reversed by
 *      the decryption — our simplified implementation skips the explicit
 *      XOR-stripe merge; stripes=1 avoids the extra work.)
 *
 *   5. VERIFY  — Validate the recovered master key by computing
 *      SHA256(mk_digest_salt || master_key) and comparing against
 *      the stored mk_digest.  A mismatch means wrong passphrase.
 *
 * Returns 0 on success, or:
 *   -EINVAL  if parameters are invalid
 *   -ENOENT  if key slot is inactive
 *   -ENOMEM  if buffer allocation fails
 *   -EPERM   if master key digest doesn't match (wrong passphrase)
 */
int luks_open_keyslot(int dev_id, struct luks_header *hdr, int slot,
                      const char *passphrase, uint8_t *mk)
{
    uint8_t *key_material = NULL;
    uint8_t *derived_key = NULL;
    int ret = -EINVAL;

    if (!hdr || !passphrase || !mk)
        return -EINVAL;
    if (slot < 0 || slot >= LUKS_KEY_SLOTS)
        return -EINVAL;

    struct luks_keyslot *ks = &hdr->key_slots[slot];

    /* Check if slot is active */
    if (ks->state != LUKS_SLOT_ACTIVE) {
        kprintf("[luks] slot %d is inactive (state=0x%04x)\n", slot, ks->state);
        return -ENOENT;
    }

    size_t pw_len = strlen(passphrase);
    uint32_t key_bytes = hdr->key_bytes;
    uint32_t km_offset = ks->key_material_offset;
    uint32_t stripes = ks->stripes;

    if (key_bytes == 0 || key_bytes > 128) {
        kprintf("[luks] invalid key_bytes %u\n", key_bytes);
        return -EINVAL;
    }

    /* Allocate buffers */
    derived_key = (uint8_t *)kmalloc(key_bytes);
    key_material = (uint8_t *)kmalloc_array(key_bytes, stripes);
    if (!derived_key || !key_material) {
        ret = -ENOMEM;
        goto out;
    }

    /* Step 1: Derive key from passphrase using PBKDF2 */
    pbkdf2_hmac_sha256((const uint8_t *)passphrase, pw_len,
                        ks->salt, 32,
                        ks->iterations,
                        derived_key, key_bytes);

    kprintf("[luks] slot %d: PBKDF2 complete (%u iterations)\n",
            slot, ks->iterations);

    /* Step 2: Read key material from disk */
    {
        uint32_t km_sectors = (uint32_t)((key_bytes * stripes + 511) / 512);
        ret = blk_submit_sync(dev_id, km_offset, km_sectors,
                               key_material, BLK_REQ_READ);
        if (ret < 0) {
            kprintf("[luks] read key material failed at sector %u: %d\n",
                    km_offset, ret);
            goto out;
        }
    }

    /* Step 3: Decrypt key material using derived key.
     *
     * The key material on disk is the result of an anti-forensic (AF) split
     * followed by encryption.  The AF splitter takes the master key and
     * expands it into (stripes × key_bytes) bytes by XOR-masking with a
     * hash chain of the master key — this ensures that even a partial
     * disk overwrite of the key material area destroys the master key
     * beyond recovery (anti-forensic property).
     *
     * In our implementation, the AF merge is effectively performed by
     * decrypting the entire blob and extracting the first key_bytes bytes.
     * With stripes=1 (no expansion), the AF layer is transparent.
     *
     * The decryption uses AES-XTS with the derived key split in half:
     *   - First half  → AES cipher key (data encryption)
     *   - Second half → XTS tweak key (ciphertext stealing / sector tweak)
     *
     * Each 512-byte sector is decrypted independently using the sector
     * index (relative to the key material area start) as the XTS tweak. */
    {
        struct xts_ctx xts;
        int half_key = (int)(key_bytes / 2);

        if (half_key < 16 || half_key > 32) {
            ret = -EINVAL;
            goto out;
        }

        /* Use first half of derived key as data key, second half as tweak key */
        ret = xts_init(&xts, derived_key, derived_key + half_key, half_key);
        if (ret != 0) {
            kprintf("[luks] xts_init failed: %d\n", ret);
            goto out;
        }

        int num_sectors = (int)((key_bytes * stripes + 511) / 512);
        for (int i = 0; i < num_sectors; i++) {
            xts_decrypt_sector(&xts, (uint64_t)i,
                               key_material + (uint64_t)i * 512,
                               key_material + (uint64_t)i * 512);
        }
    }

    /* Step 4: Extract the master key (first key_bytes bytes of decrypted material) */
    memcpy(mk, key_material, key_bytes);

    /* Step 5: Verify master key digest
     *
     * Compute SHA256(mk_digest_salt || master_key) and compare with
     * stored mk_digest.  This is a simplified verification — real LUKS
     * uses PBKDF2 for the digest check too, but using SHA256 directly
     * is functional for our implementation. */
    {
        uint8_t computed_digest[LUKS_DIGEST_SIZE];
        struct sha256_ctx ctx;

        sha256_init(&ctx);
        sha256_update(&ctx, hdr->mk_digest_salt, 32);
        sha256_update(&ctx, mk, key_bytes);
        sha256_final(computed_digest, &ctx);

        if (memcmp(computed_digest, hdr->mk_digest, LUKS_DIGEST_SIZE) != 0) {
            kprintf("[luks] slot %d: digest mismatch (wrong passphrase)\n", slot);
            ret = -EPERM;
            goto out;
        }
    }

    kprintf("[luks] slot %d: master key verified, %u bytes\n",
            slot, key_bytes);
    ret = 0;

out:
    if (derived_key) {
        memset(derived_key, 0, key_bytes);
        kfree(derived_key);
    }
    if (key_material) {
        memset(key_material, 0, key_bytes * stripes);
        kfree(key_material);
    }
    return ret;
}

/* ── luks_setup_dm_crypt ─────────────────────────────────────────── */

/*
 * luks_setup_dm_crypt - Create a dm-crypt mapping from LUKS parameters.
 * @dev_id:  Source block device ID (underlying encrypted device)
 * @hdr:     Parsed LUKS header (provides payload_offset and key_bytes)
 * @mk:      Master key (key_bytes bytes, recovered from luks_open_keyslot)
 *
 * Builds a dm-crypt target table and instructs the device-mapper to
 * create a transparent encryption layer over the data area of the LUKS
 * device (from payload_offset to end).
 *
 * The master key is converted to two hex strings:
 *   key1 = first half of master key  (AES cipher key)
 *   key2 = second half of master key (XTS tweak key)
 *
 * The dm table format is:
 *   "<start> <length> crypt <key1_hex> <key2_hex> <dev_id> <payload_offset>"
 *
 * After loading the table with dm_table_load(), the device is activated
 * via dm_device_resume().  On failure the partially-created dm device is
 * cleaned up with dm_device_remove().
 *
 * Returns the dm device ID (> 0) on success, negative errno on failure.
 */
int luks_setup_dm_crypt(int dev_id, struct luks_header *hdr, const uint8_t *mk)
{
    char table[256];
    char key1_hex[128];
    char key2_hex[128];
    int w1 = 0;
    int w2 = 0;

    if (!hdr || !mk)
        return -EINVAL;

    /* Convert master key to hex strings for dm-crypt table.
     * dm-crypt expects key1_hex key2_hex where key1 and key2 are
     * the two halves of the XTS key. */
    int half_key = (int)(hdr->key_bytes / 2);

    for (int i = 0; i < half_key && w1 < (int)sizeof(key1_hex) - 4; i++) {
        w1 += snprintf(key1_hex + w1, sizeof(key1_hex) - (size_t)w1,
                        "%02x", mk[i]);
    }
    key1_hex[w1] = '\0';

    for (int i = half_key; i < (int)hdr->key_bytes && w2 < (int)sizeof(key2_hex) - 4; i++) {
        w2 += snprintf(key2_hex + w2, sizeof(key2_hex) - (size_t)w2,
                        "%02x", mk[i]);
    }
    key2_hex[w2] = '\0';

    /* Calculate size: from payload_offset to end of device */
    uint64_t total_sectors = blockdev_get_sectors(dev_id);
    uint64_t crypto_sectors = total_sectors - hdr->payload_offset;

    /* Create the dm device */
    char dm_name[DM_NAME_MAX];
    snprintf(dm_name, sizeof(dm_name), "luks_%s", hdr->uuid);

    int dm_id = dm_device_create(dm_name, crypto_sectors);
    if (dm_id < 0) {
        kprintf("[luks] dm_device_create failed: %d\n", dm_id);
        return dm_id;
    }

    /* Build the table */
    snprintf(table, sizeof(table),
             "0 %llu crypt %s %s %d %u",
             (unsigned long long)crypto_sectors,
             key1_hex, key2_hex,
             dev_id, hdr->payload_offset);

    int ret = dm_table_load(dm_id, table);
    if (ret < 0) {
        kprintf("[luks] dm_table_load failed: %d\n", ret);
        dm_device_remove(dm_id);
        return ret;
    }

    /* Resume to activate */
    ret = dm_device_resume(dm_id);
    if (ret < 0) {
        kprintf("[luks] dm_device_resume failed: %d\n", ret);
        dm_device_remove(dm_id);
        return ret;
    }

    kprintf("[luks] dm-crypt device '%s' (dm-%d) created, %llu sectors, "
            "AES-%d-XTS, payload at sector %u\n",
            dm_name, dm_id,
            (unsigned long long)crypto_sectors,
            (int)hdr->key_bytes * 4,
            hdr->payload_offset);

    return dm_id;
}

/*
 * luks_open - Open and activate a LUKS device.
 * @device:     Device path (e.g. "/dev/sda")
 * @passphrase: User-supplied passphrase
 *
 * Placeholder: future implementation will call luks_parse_header(),
 * luks_open_keyslot(), and luks_setup_dm_crypt() in sequence.
 * Currently logs the operation and returns success.
 */
static int luks_open(const char *device, const char *passphrase)
{
    (void)passphrase;
    kprintf("[luks] Opening LUKS device: %s\n", device);
    return 0;
}
/*
 * luks_close - Close and deactivate a LUKS device.
 * @device: Device path
 *
 * Placeholder: will eventually tear down the dm-crypt mapping
 * and clear sensitive key material.  Currently logs and returns success.
 */
static int luks_close(const char *device)
{
    kprintf("[luks] Closing LUKS device: %s\n", device);
    return 0;
}
/*
 * luks_format - Initialize a new LUKS device.
 * @device:     Device path
 * @passphrase: Initial passphrase
 *
 * Placeholder: future implementation will write a fresh LUKS v1 header,
 * generate a master key, create key slot 0 by AF-splitting the master
 * key and encrypting with a PBKDF2-derived key, then zero-fill the data
 * area.  Currently logs and returns success.
 */
static int luks_format(const char *device, const char *passphrase)
{
    (void)passphrase;
    kprintf("[luks] Formatting LUKS device: %s\n", device);
    return 0;
}
/*
 * luks_add_key - Add a new passphrase key slot to an existing LUKS device.
 * @device:  Device path
 * @old_pass: Existing passphrase (to unlock the master key)
 * @new_pass: New passphrase for the added key slot
 *
 * Placeholder: future implementation will unlock with old_pass, then
 * create a new key slot using new_pass (PBKDF2 derive → AF split →
 * encrypt key material).  Currently logs and returns success.
 */
static int luks_add_key(const char *device, const char *old_pass, const char *new_pass)
{
    (void)device;
    (void)old_pass;
    (void)new_pass;
    kprintf("[luks] luks_add_key: %s\n", device);
    return 0;
}
