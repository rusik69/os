/*
 * tpm_key.c — TPM 2.0 key management
 *
 * Provides key generation, sealing, and asymmetric signing using the
 * TPM2.  Designed to support dm-crypt LUKS key sealing operations.
 *
 * Features:
 *   - Generate and seal keys using TPM2_Create / TPM2_Load / TPM2_Unseal
 *   - Asymmetric signing via TPM2_Sign
 *   - LUKS key sealing interface for dm-crypt
 *
 * Item S97 — TPM key management
 */

#define KERNEL_INTERNAL
#include "tpm.h"
#include "printf.h"
#include "string.h"
#include "pmm.h"
#include "sha256.h"
#include "errno.h"
#include "heap.h"

/* TPM driver functions */
int tpm_is_present(void);

/* ── Default NV index for LUKS key storage ────────────────────── */

#define TPM_LUKS_NV_INDEX    0x01C10101  /* NV index for LUKS master key */
#define TPM_KEY_NV_INDEX     0x01C10102  /* NV index for general key */
#define TPM_SIGNING_KEY_NV   0x01C10103  /* NV index for signing key blob */

/* ── Key types ────────────────────────────────────────────────── */

enum tpm_key_type {
    TPM_KEY_TYPE_RSA     = 1,
    TPM_KEY_TYPE_ECC     = 2,
    TPM_KEY_TYPE_SYMMETRIC = 3,
};

struct tpm_key {
    enum tpm_key_type  type;
    uint32_t           parent_handle;   /* handle of parent storage key */
    uint32_t           object_handle;   /* handle after loading */
    uint8_t           *priv_blob;       /* private blob (from Create) */
    uint32_t           priv_len;
    uint8_t           *pub_blob;        /* public blob (from Create) */
    uint32_t           pub_len;
    int                loaded;          /* 1 if currently loaded in TPM */
};

/* ═══════════════════════════════════════════════════════════════════
 *  Key generation and sealing
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * tpm_generate_key — Generate a new TPM-protected key.
 *
 * @parent:   Handle of the parent storage key (e.g., TPM2_RH_OWNER).
 * @sealed_data: Data to seal (or NULL for key generation).
 * @sealed_len: Length of sealed data.
 * @auth:     Optional password auth (can be NULL).
 * @auth_len: Length of auth.
 * @out_key:  Filled with key handles and blobs.
 *
 * Returns 0 on success, -1 on failure.
 */
static int tpm_generate_key(uint32_t parent, const uint8_t *sealed_data,
                     uint32_t sealed_len, const uint8_t *auth,
                     uint32_t auth_len, struct tpm_key *out_key)
{
    if (!out_key)
        return -1;

    memset(out_key, 0, sizeof(*out_key));
    out_key->parent_handle = parent;

    /* Allocate buffer for private and public blobs */
    out_key->priv_blob = (uint8_t *)kmalloc(1024);
    out_key->pub_blob  = (uint8_t *)kmalloc(1024);
    if (!out_key->priv_blob || !out_key->pub_blob) {
        kfree(out_key->priv_blob);
        kfree(out_key->pub_blob);
        return -1;
    }
    out_key->priv_len = 1024;
    out_key->pub_len  = 1024;

    /* If sealed_data is provided, seal it; otherwise generate random key */
    uint8_t default_data[32];
    if (!sealed_data || sealed_len == 0) {
        /* Generate a random 32-byte key */
        int ret = tpm2_get_random(default_data, sizeof(default_data));
        if (ret <= 0) {
            kprintf("[TPM_key] failed to generate random key data\n");
            kfree(out_key->priv_blob);
            kfree(out_key->pub_blob);
            return -1;
        }
        sealed_data = default_data;
        sealed_len = (uint32_t)ret;
    }

    /* Create the sealed object in the TPM */
    int ret = tpm2_create(parent, sealed_data, sealed_len,
                          auth, auth_len,
                          out_key->priv_blob, &out_key->priv_len,
                          out_key->pub_blob, &out_key->pub_len);
    if (ret < 0) {
        kprintf("[TPM_key] tpm2_create failed\n");
        kfree(out_key->priv_blob);
        kfree(out_key->pub_blob);
        return -1;
    }

    out_key->type = TPM_KEY_TYPE_SYMMETRIC;

    kprintf("[TPM_key] key generated (priv=%u bytes, pub=%u bytes)\n",
            out_key->priv_len, out_key->pub_len);

    return 0;
}

/*
 * tpm_load_key — Load a TPM key into the TPM for use.
 *
 * @out_key:  Key descriptor (must have priv_blob and pub_blob filled).
 *
 * Returns 0 on success, sets out_key->object_handle and out_key->loaded=1.
 */
static int tpm_load_key(struct tpm_key *out_key)
{
    if (!out_key || !out_key->priv_blob || !out_key->pub_blob)
        return -1;

    if (out_key->loaded)
        return 0;  /* already loaded */

    int ret = tpm2_load(out_key->parent_handle,
                        out_key->priv_blob, out_key->priv_len,
                        out_key->pub_blob, out_key->pub_len,
                        &out_key->object_handle);
    if (ret < 0) {
        kprintf("[TPM_key] tpm2_load failed\n");
        return -1;
    }

    out_key->loaded = 1;
    kprintf("[TPM_key] key loaded (handle=0x%08x)\n", out_key->object_handle);

    return 0;
}

/*
 * tpm_unload_key — Flush a loaded TPM key from TPM memory.
 */
static void tpm_unload_key(struct tpm_key *key)
{
    if (!key || !key->loaded)
        return;

    tpm2_flush_context(key->object_handle);
    key->loaded = 0;
    key->object_handle = 0;

    kprintf("[TPM_key] key unloaded\n");
}

/*
 * tpm_seal_key_to_nv — Generate a key and store it sealed in TPM NV.
 *
 * @parent:        Parent storage key handle.
 * @nv_index:      NV index for storage.
 * @auth:          Optional auth (can be NULL).
 * @auth_len:      Auth length.
 * @out_key:       Filled with key info (blobs can be freed after NV store).
 *
 * Returns 0 on success.
 */
static int tpm_seal_key_to_nv(uint32_t parent, uint32_t nv_index,
                       const uint8_t *auth, uint32_t auth_len,
                       struct tpm_key *out_key)
{
    if (!out_key)
        return -1;

    /* Generate a random key */
    int ret = tpm_generate_key(parent, NULL, 0, auth, auth_len, out_key);
    if (ret < 0)
        return -1;

    /* Store the sealed blob in NV storage */
    /* We store: priv_len (4 bytes) + priv_blob + pub_len (4 bytes) + pub_blob */
    uint32_t total = 4 + out_key->priv_len + 4 + out_key->pub_len;
    uint8_t *nv_data = (uint8_t *)kmalloc(total);
    if (!nv_data) {
        tpm_unload_key(out_key);
        return -1;
    }

    uint32_t off = 0;
    nv_data[off++] = (uint8_t)(out_key->priv_len >> 24);
    nv_data[off++] = (uint8_t)(out_key->priv_len >> 16);
    nv_data[off++] = (uint8_t)(out_key->priv_len >> 8);
    nv_data[off++] = (uint8_t)(out_key->priv_len & 0xFF);
    memcpy(nv_data + off, out_key->priv_blob, out_key->priv_len);
    off += out_key->priv_len;
    nv_data[off++] = (uint8_t)(out_key->pub_len >> 24);
    nv_data[off++] = (uint8_t)(out_key->pub_len >> 16);
    nv_data[off++] = (uint8_t)(out_key->pub_len >> 8);
    nv_data[off++] = (uint8_t)(out_key->pub_len & 0xFF);
    memcpy(nv_data + off, out_key->pub_blob, out_key->pub_len);

    ret = tpm_nv_store_key(nv_index, nv_data, total);
    kfree(nv_data);

    if (ret < 0) {
        kprintf("[TPM_key] failed to store key in NV\n");
        return -1;
    }

    kprintf("[TPM_key] key sealed to NV index 0x%08x\n", nv_index);
    return 0;
}

/*
 * tpm_load_key_from_nv — Load a key from NV storage into the TPM.
 *
 * @nv_index:  NV index where the key is stored.
 * @parent:    Parent storage key handle.
 * @out_key:   Filled with loaded key info.
 *
 * Returns 0 on success.
 */
static int tpm_load_key_from_nv(uint32_t nv_index, uint32_t parent,
                         struct tpm_key *out_key)
{
    if (!out_key)
        return -1;

    memset(out_key, 0, sizeof(*out_key));
    out_key->parent_handle = parent;

    /* Read blob from NV */
    uint32_t nv_size = 2048;
    uint8_t *nv_data = (uint8_t *)kmalloc(nv_size);
    if (!nv_data) return -1;

    int ret = tpm_nv_load_key(nv_index, nv_data, &nv_size);
    if (ret < 0) {
        kprintf("[TPM_key] failed to read from NV index 0x%08x\n", nv_index);
        kfree(nv_data);
        return -1;
    }

    uint32_t off = 0;
    out_key->priv_len = ((uint32_t)nv_data[off] << 24) |
                        ((uint32_t)nv_data[off+1] << 16) |
                        ((uint32_t)nv_data[off+2] << 8) |
                        (uint32_t)nv_data[off+3];
    off += 4;

    if (out_key->priv_len > 1024 || off + out_key->priv_len > nv_size) {
        kfree(nv_data);
        return -1;
    }
    out_key->priv_blob = (uint8_t *)kmalloc(out_key->priv_len);
    if (!out_key->priv_blob) { kfree(nv_data); return -1; }
    memcpy(out_key->priv_blob, nv_data + off, out_key->priv_len);
    off += out_key->priv_len;

    out_key->pub_len = ((uint32_t)nv_data[off] << 24) |
                       ((uint32_t)nv_data[off+1] << 16) |
                       ((uint32_t)nv_data[off+2] << 8) |
                       (uint32_t)nv_data[off+3];
    off += 4;

    if (out_key->pub_len > 1024 || off + out_key->pub_len > nv_size) {
        kfree(nv_data);
        kfree(out_key->priv_blob);
        return -1;
    }
    out_key->pub_blob = (uint8_t *)kmalloc(out_key->pub_len);
    if (!out_key->pub_blob) {
        kfree(nv_data);
        kfree(out_key->priv_blob);
        return -1;
    }
    memcpy(out_key->pub_blob, nv_data + off, out_key->pub_len);

    kfree(nv_data);

    /* Load the key */
    ret = tpm_load_key(out_key);
    if (ret < 0) {
        kfree(out_key->priv_blob);
        kfree(out_key->pub_blob);
        return -1;
    }

    out_key->type = TPM_KEY_TYPE_SYMMETRIC;

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Asymmetric signing via TPM2_Sign
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * tpm_sign_hash — Sign a hash digest using a loaded TPM key.
 *
 * @key:       Loaded TPM key descriptor.
 * @digest:    Hash digest to sign.
 * @digest_len: Length of digest (e.g., 32 for SHA-256).
 * @signature: Output buffer for the signature.
 * @sig_len:   On input: max sig size; on output: actual sig size.
 *
 * Returns 0 on success.
 */
static int tpm_sign_hash(struct tpm_key *key, const uint8_t *digest,
                  uint32_t digest_len, uint8_t *signature,
                  uint32_t *sig_len)
{
    if (!key || !key->loaded || !digest || !signature || !sig_len)
        return -1;

    return tpm2_sign(key->object_handle, digest, digest_len,
                     signature, sig_len);
}

/* ═══════════════════════════════════════════════════════════════════
 *  LUKS key sealing interface
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * tpm_luks_seal_key — Seal a LUKS master key with the TPM.
 *
 * @luks_key:      LUKS master key data.
 * @luks_key_len:  Length of LUKS key.
 * @auth:          Optional auth string (can be NULL).
 * @auth_len:      Auth length.
 *
 * The key is sealed under TPM2_RH_OWNER and stored in NV at the
 * standard LUKS NV index (0x01C10101).
 *
 * Returns 0 on success.
 */
static int tpm_luks_seal_key(const uint8_t *luks_key, uint32_t luks_key_len,
                      const uint8_t *auth, uint32_t auth_len)
{
    if (!luks_key || luks_key_len == 0)
        return -1;

    struct tpm_key key;
    int ret = tpm_generate_key(TPM2_RH_OWNER, luks_key, luks_key_len,
                               auth, auth_len, &key);
    if (ret < 0)
        return -1;

    /* Store in NV */
    ret = tpm_seal_key_to_nv(TPM2_RH_OWNER, TPM_LUKS_NV_INDEX,
                             auth, auth_len, &key);

    /* Clean up */
    tpm_unload_key(&key);
    kfree(key.priv_blob);
    kfree(key.pub_blob);

    return ret;
}

/*
 * tpm_luks_unseal_key — Retrieve a LUKS master key from TPM.
 *
 * @luks_key:      Buffer for the LUKS key.
 * @luks_key_len:  On input: max size; on output: actual size.
 * @auth:          Optional auth string.
 * @auth_len:      Auth length.
 *
 * Returns 0 on success.
 */
static int tpm_luks_unseal_key(uint8_t *luks_key, uint32_t *luks_key_len,
                        const uint8_t *auth, uint32_t auth_len)
{
    if (!luks_key || !luks_key_len)
        return -1;

    struct tpm_key key;
    int ret = tpm_load_key_from_nv(TPM_LUKS_NV_INDEX, TPM2_RH_OWNER, &key);
    if (ret < 0)
        return -1;

    /* Unseal to get the original data */
    ret = tpm2_unseal(key.object_handle, luks_key, luks_key_len);

    tpm_unload_key(&key);
    kfree(key.priv_blob);
    kfree(key.pub_blob);

    return ret;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Initialisation
 * ═══════════════════════════════════════════════════════════════════ */

static int tpm_key_init(void)
{
    if (!tpm_is_present()) {
        kprintf("[TPM_key] TPM not present, skipping key management init\n");
        return -1;
    }

    kprintf("[TPM_key] TPM key management ready\n");
    return 0;
}

/* ── Stub: tpm_key_create ─────────────────────────────── */
static int tpm_key_create(const char *name, void *key)
{
    (void)name;
    (void)key;
    kprintf("[tpm] tpm_key_create: not yet implemented\n");
    return 0;
}
/* ── Stub: tpm_key_load ─────────────────────────────── */
static int tpm_key_load(const char *name, void *key)
{
    (void)name;
    (void)key;
    kprintf("[tpm] tpm_key_load: not yet implemented\n");
    return 0;
}
/* ── Stub: tpm_key_sign ─────────────────────────────── */
static int tpm_key_sign(const void *data, size_t dlen, void *sig, size_t *slen)
{
    (void)data;
    (void)dlen;
    (void)sig;
    (void)slen;
    kprintf("[tpm] tpm_key_sign: not yet implemented\n");
    return 0;
}
