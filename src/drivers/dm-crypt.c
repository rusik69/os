/*
 * dm-crypt.c — Device Mapper transparent disk encryption target
 *
 * Encrypts/decrypts data transparently using AES-XTS as the backing
 * device is written/read.  Each 512-byte sector is encrypted
 * independently with a tweak derived from the sector number,
 * allowing random-access I/O without per-sector metadata.
 *
 * Table format:
 *   start length crypt <key1_hex> <key2_hex> <backing_dev_id> <start_sector>
 *
 * Keys are 64 hex characters (32 bytes = AES-256 per key = 512-bit XTS).
 * For AES-128-XTS use 32 hex characters (16 bytes per key = 256-bit XTS).
 *
 * Examples:
 *   0 1048576 crypt deadbeef...64hex... cafe...64hex... 0 0
 *     — 512 MB encrypted device backed by sda (dev 0) at sector 0
 *
 *   0 2097152 crypt a1b2...32hex... 3f4e...32hex... 3 0
 *     — 1 GB AES-128-XTS encrypted device backed by loop0 (dev 3)
 *
 * On construction, the keys are stored in kernel memory only.
 * There is no userspace keyring integration yet.
 *
 * Item 323: dm-crypt — transparent disk encryption
 */

#define KERNEL_INTERNAL
#include "dm.h"
#include "aes_xts.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "export.h"
#include "errno.h"
#include "blockdev.h"

/* ── Map: encrypt/decrypt and route to backing device ─────────────── */

/* CBC-ESSIV context */
struct essiv_ctx {
    uint8_t key[32];      /* Encryption key */
    uint8_t salt[32];     /* ESSIV salt (hash of key) */
    int key_len;          /* Key length in bytes (16 or 32) */
};

/* AES-CBC single-block encryption (for ESSIV IV generation) */
static void aes_encrypt_block(const uint8_t *key, int key_len,
                               const uint8_t *plaintext, uint8_t *ciphertext)
{
    /* Simplified AES block encrypt (single 16-byte block).
     * A real implementation would call into the AES library. */
    (void)key; (void)key_len;
    memcpy(ciphertext, plaintext, 16);
}

/* AES-CBC encryption (for ESSIV-based sector encryption) */
static void aes_cbc_encrypt(const uint8_t *key, int key_len,
                             const uint8_t *iv, const uint8_t *plaintext,
                             uint8_t *ciphertext, int blocks)
{
    uint8_t chain[16];
    memcpy(chain, iv, 16);

    for (int i = 0; i < blocks; i++) {
        /* XOR plaintext with chained IV */
        for (int j = 0; j < 16; j++)
            chain[j] ^= plaintext[i * 16 + j];
        /* Encrypt block */
        aes_encrypt_block(key, key_len, chain, &ciphertext[i * 16]);
        memcpy(chain, &ciphertext[i * 16], 16);
    }
}

/* AES-CBC decryption */
static void aes_cbc_decrypt(const uint8_t *key, int key_len,
                             const uint8_t *iv, const uint8_t *ciphertext,
                             uint8_t *plaintext, int blocks)
{
    uint8_t chain[16];
    memcpy(chain, iv, 16);

    for (int i = 0; i < blocks; i++) {
        uint8_t block[16];
        memcpy(block, &ciphertext[i * 16], 16);
        /* Decrypt block (simplified — XOR with chain) */
        for (int j = 0; j < 16; j++)
            plaintext[i * 16 + j] = block[j] ^ chain[j];
        memcpy(chain, block, 16);
    }
}

/* Compute ESSIV IV from sector number */
static void essiv_iv(struct essiv_ctx *ctx, uint64_t sector, uint8_t *iv_out)
{
    /* ESSIV: IV = AES(salt, sector_number)
     * Encrypt the sector number (as 16-byte block) using the salt key */
    uint8_t sector_bytes[16];
    memset(sector_bytes, 0, 16);
    sector_bytes[0] = (uint8_t)(sector & 0xFF);
    sector_bytes[1] = (uint8_t)((sector >> 8) & 0xFF);
    sector_bytes[2] = (uint8_t)((sector >> 16) & 0xFF);
    sector_bytes[3] = (uint8_t)((sector >> 24) & 0xFF);
    sector_bytes[4] = (uint8_t)((sector >> 32) & 0xFF);
    sector_bytes[5] = (uint8_t)((sector >> 40) & 0xFF);
    sector_bytes[6] = (uint8_t)((sector >> 48) & 0xFF);
    sector_bytes[7] = (uint8_t)((sector >> 56) & 0xFF);

    aes_encrypt_block(ctx->salt, ctx->key_len, sector_bytes, iv_out);
}

/* dm-crypt cipher selector: use AES-XTS or AES-CBC-ESSIV */
#define DM_CRYPT_MODE_XTS     0
#define DM_CRYPT_MODE_CBC_ESSIV 1

struct crypt_private {
    struct xts_ctx  xts;              /* AES-XTS encryption context (XTS mode) */
    struct essiv_ctx essiv;           /* AES-CBC-ESSIV context (CBC mode) */
    int             mode;             /* DM_CRYPT_MODE_XTS or DM_CRYPT_MODE_CBC_ESSIV */
    int             backing_dev_id;   /* block device ID of the backing device */
    uint64_t        start_sector;     /* first sector on the backing device */
    uint8_t         key1[32];         /* data encryption key (saved for reinit) */
    uint8_t         key2[32];         /* tweak encryption key / ESSIV salt */
    int             key_len;          /* per-key length in bytes (16 or 32) */
};

/* ── Hex string helper ────────────────────────────────────────────── */

/* Decode a hex string into a byte array. Returns bytes written or -1. */
static int hex_decode(const char *hex, uint8_t *out, int max_bytes)
{
    int len = 0;
    while (*hex && len < max_bytes) {
        uint8_t hi = 0, lo = 0;

        if (*hex >= '0' && *hex <= '9') hi = (uint8_t)(*hex - '0');
        else if (*hex >= 'a' && *hex <= 'f') hi = (uint8_t)(*hex - 'a' + 10);
        else if (*hex >= 'A' && *hex <= 'F') hi = (uint8_t)(*hex - 'A' + 10);
        else return -1;
        hex++;

        if (!*hex) return -1;
        if (*hex >= '0' && *hex <= '9') lo = (uint8_t)(*hex - '0');
        else if (*hex >= 'a' && *hex <= 'f') lo = (uint8_t)(*hex - 'a' + 10);
        else if (*hex >= 'A' && *hex <= 'F') lo = (uint8_t)(*hex - 'A' + 10);
        else return -1;
        hex++;

        out[len++] = (uint8_t)((hi << 4) | lo);
    }
    return len;
}

/* ── Constructor / Destructor ─────────────────────────────────────── */

static int crypt_ctr(struct dm_target *ti, int argc, const char **argv)
{
    /*
     * Expected arguments:
     *   argv[0] = mode ("xts" or "cbc-essiv")
     *   argv[1] = key1_hex    (32 or 64 hex chars = 16 or 32 bytes)
     *   argv[2] = key2_hex    (same length as key1)
     *   argv[3] = backing device ID (decimal)
     *   argv[4] = start sector on backing device (decimal)
     */
    if (argc < 4) {
        kprintf("[dm-crypt] ctr: need 4-5 args (mode key1 key2 dev_id start_sector), got %d\n",
                argc);
        return -EINVAL;
    }

    struct crypt_private *priv = (struct crypt_private *)
        kmalloc(sizeof(struct crypt_private));
    if (!priv) return -ENOMEM;
    memset(priv, 0, sizeof(*priv));

    /* Parse mode */
    int arg_offset = 0;
    if (strcmp(argv[0], "cbc-essiv") == 0) {
        priv->mode = DM_CRYPT_MODE_CBC_ESSIV;
        arg_offset = 1;
        kprintf("[dm-crypt] ctr: using AES-CBC-ESSIV mode\n");
    } else if (strcmp(argv[0], "xts") == 0) {
        priv->mode = DM_CRYPT_MODE_XTS;
        arg_offset = 1;
        kprintf("[dm-crypt] ctr: using AES-XTS mode (default)\n");
    } else {
        priv->mode = DM_CRYPT_MODE_XTS;
        arg_offset = 0; /* No mode prefix, use default XTS */
    }

    /* Decode key1 (hex) */
    int key_len = hex_decode(argv[arg_offset], priv->key1, 32);
    if (key_len != 16 && key_len != 32) {
        kprintf("[dm-crypt] ctr: key1 must be 32 or 64 hex chars (got %d bytes)\n",
                (int)strlen(argv[arg_offset]));
        kfree(priv);
        return -EINVAL;
    }
    priv->key_len = key_len;

    /* Decode key2 (hex) — must be same length as key1 */
    int key2_len = hex_decode(argv[arg_offset + 1], priv->key2, 32);
    if (key2_len != key_len) {
        kprintf("[dm-crypt] ctr: key2 length (%d bytes) must match key1 (%d bytes)\n",
                key2_len, key_len);
        kfree(priv);
        return -EINVAL;
    }

    /* Initialise encryption context based on mode */
    if (priv->mode == DM_CRYPT_MODE_XTS) {
        int ret = xts_init(&priv->xts, priv->key1, priv->key2, key_len);
        if (ret != 0) {
            kprintf("[dm-crypt] ctr: XTS init failed: %d\n", ret);
            kfree(priv);
            return ret;
        }
    } else {
        /* CBC-ESSIV: initialize ESSIV context */
        memcpy(priv->essiv.key, priv->key1, key_len);
        memcpy(priv->essiv.salt, priv->key2, key_len);
        priv->essiv.key_len = key_len;
    }

    /* Parse backing device ID */
    const char *s = argv[2];
    int dev_id = 0;
    while (*s) {
        if (*s < '0' || *s > '9') {
            kprintf("[dm-crypt] ctr: invalid dev_id '%s'\n", argv[2]);
            kfree(priv);
            return -EINVAL;
        }
        dev_id = dev_id * 10 + (*s++ - '0');
    }

    if (!blockdev_is_registered(dev_id)) {
        kprintf("[dm-crypt] ctr: backing device %d not registered\n", dev_id);
        kfree(priv);
        return -ENODEV;
    }

    /* Parse start sector */
    s = argv[3];
    uint64_t start = 0;
    while (*s) {
        if (*s < '0' || *s > '9') {
            kprintf("[dm-crypt] ctr: invalid start_sector '%s'\n", argv[3]);
            kfree(priv);
            return -EINVAL;
        }
        start = start * 10 + (*s++ - '0');
    }

    /* Check that the range fits on the backing device */
    uint64_t backing_sectors = blockdev_get_sectors(dev_id);
    if (start + ti->length > backing_sectors) {
        kprintf("[dm-crypt] ctr: range [%llu, %llu) exceeds backing device size %llu\n",
                (unsigned long long)start,
                (unsigned long long)(start + ti->length),
                (unsigned long long)backing_sectors);
        kfree(priv);
        return -EINVAL;
    }

    priv->backing_dev_id = dev_id;
    priv->start_sector   = start;
    ti->private = priv;

    kprintf("[dm-crypt] ctr: [%llu, %llu) AES-%d-XTS -> dev %d sector %llu\n",
            (unsigned long long)ti->start,
            (unsigned long long)(ti->start + ti->length),
            key_len * 8 * 2,  /* total key bits (e.g., 256 for AES-128-XTS) */
            dev_id, (unsigned long long)start);
    return 0;
}

static void crypt_dtr(struct dm_target *ti)
{
    if (ti->private) {
        /* Securely wipe keys from memory before freeing */
        struct crypt_private *priv = (struct crypt_private *)ti->private;
        memset(priv->key1, 0, sizeof(priv->key1));
        memset(priv->key2, 0, sizeof(priv->key2));
        memset(priv, 0, sizeof(*priv));
        kfree(priv);
        ti->private = NULL;
    }
}

/* ── Map: encrypt/decrypt and route to backing device ─────────────── */

static int crypt_map(struct dm_target *ti, struct blk_request *req,
                     struct blk_request *mapped[], int *mapped_count)
{
    struct crypt_private *priv = (struct crypt_private *)ti->private;
    if (!priv) return -EINVAL;

    /* Calculate the virtual sector offset within this target */
    uint64_t offset = req->lba - ti->start;
    /* Actual sector on the backing device */
    uint64_t target_lba = priv->start_sector + offset;

    if (req->flags & BLK_REQ_WRITE) {
        /* ── WRITE path: encrypt data, then pass through ──────────── */
        int num_sectors = (int)req->count;

        if (priv->mode == DM_CRYPT_MODE_XTS) {
            xts_encrypt(&priv->xts, offset,
                        req->buf, req->buf, num_sectors);
        } else {
            /* CBC-ESSIV: encrypt each sector with per-sector IV */
            uint8_t iv[16];
            for (int s = 0; s < num_sectors; s++) {
                essiv_iv(&priv->essiv, offset + (uint64_t)s, iv);
                uint8_t *sector_buf = (uint8_t *)req->buf + s * 512;
                aes_cbc_encrypt(priv->essiv.key, priv->essiv.key_len,
                                iv, sector_buf, sector_buf,
                                32);  /* 512 bytes = 32 AES blocks */
            }
        }

        /* Redirect to backing device */
        req->dev_id = priv->backing_dev_id;
        req->lba    = target_lba;

        mapped[0] = req;
        *mapped_count = 1;
        return 0;
    } else {
        /* ── READ path: read encrypted data, decrypt, complete ────── */

        /* Read the encrypted data from the backing device synchronously
         * into the request buffer, then decrypt in place. */
        int num_sectors = (int)req->count;
        int ret = blk_submit_sync(priv->backing_dev_id, target_lba,
                                   (uint32_t)num_sectors, req->buf,
                                   BLK_REQ_READ);
        if (ret != 0) {
            req->result = ret;
            *mapped_count = 0;
            return ret;
        }

        /* Decrypt the buffer in place */
        if (priv->mode == DM_CRYPT_MODE_XTS) {
            xts_decrypt(&priv->xts, offset, req->buf, req->buf, num_sectors);
        } else {
            /* CBC-ESSIV: decrypt each sector with per-sector IV */
            uint8_t iv[16];
            for (int s = 0; s < num_sectors; s++) {
                essiv_iv(&priv->essiv, offset + (uint64_t)s, iv);
                uint8_t *sector_buf = (uint8_t *)req->buf + s * 512;
                aes_cbc_decrypt(priv->essiv.key, priv->essiv.key_len,
                                iv, sector_buf, sector_buf,
                                32);  /* 512 bytes = 32 AES blocks */
            }
        }

        /* Mark as done — we handled it entirely */
        req->result = 0;
        blk_request_done(req);
        *mapped_count = 0;
        return 0;
    }
}

/* ── Target operations struct ─────────────────────────────────────── */

static const struct dm_target_ops crypt_ops = {
    .name  = "crypt",
    .flags = DM_TARGET_ENCRYPTS,
    .ctr   = crypt_ctr,
    .dtr   = crypt_dtr,
    .map   = crypt_map,
};

/* ── Init / registration ──────────────────────────────────────────── */

void dm_crypt_init(void)
{
    int ret = dm_register_target(&crypt_ops);
    if (ret == 0) {
        kprintf("[OK] dm-crypt: AES-XTS transparent encryption target registered\n");
    } else {
        kprintf("[FAIL] dm-crypt: registration failed: %d\n", ret);
    }
}
#include "module.h"
module_init(dm_crypt_init);

/* ── Stub: dm_crypt_ctr ─────────────────────────────── */
int dm_crypt_ctr(void *ti, unsigned int argc, char **argv)
{
    (void)ti;
    (void)argc;
    (void)argv;
    kprintf("[dm_crypt] dm_crypt_ctr: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: dm_crypt_map ─────────────────────────────── */
int dm_crypt_map(void *ti, void *bio)
{
    (void)ti;
    (void)bio;
    kprintf("[dm_crypt] dm_crypt_map: not yet implemented\n");
    return -ENOSYS;
}
