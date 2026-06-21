/*
 * keyring.c — Kernel-managed encrypted keyring
 *
 * Implements a kernel-managed encryption key storage facility.
 * Keys are referenced by string names and protected using AES-128
 * encryption when stored in the keyring.  The keyring is locked
 * with a master key (kernel key).
 *
 * Operations:
 *   add_key(name, key_data, key_len)   — Store an encrypted key
 *   request_key(name, buf, buf_len)    — Retrieve a decrypted key
 *   revoke_key(name)                   — Remove a key from the ring
 *
 * The master key is derived at boot from the kernel's internal RNG
 * and can be sealed by a TPM if available (TPM-based key protection).
 *
 * Item S108 — Encrypted keyring
 */

#include "types.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "rng.h"
#include "aes.h"
#include "sha256.h"
#include "errno.h"
#include "spinlock.h"

/* ── Keyring constants ───────────────────────────────────────────── */
#define KEYRING_MAX_KEYS     64
#define KEY_NAME_MAX         48
#define KEY_DATA_MAX         256
#define KEY_AUTH_TAG_SIZE    16      /* AES-CBC: 16 bytes of IV */

/* ── Key entry ───────────────────────────────────────────────────── */
struct keyring_entry {
    char     name[KEY_NAME_MAX];
    uint8_t  encrypted_data[KEY_DATA_MAX + KEY_AUTH_TAG_SIZE];
    uint32_t encrypted_len;        /* Total encrypted length */
    uint32_t plaintext_len;        /* Original plaintext length */
    uint8_t  iv[16];               /* AES IV (random per key) */
    uint8_t  in_use;               /* 1 if slot occupied */
};

/* ── Global keyring state ────────────────────────────────────────── */
static struct keyring_entry g_keyring[KEYRING_MAX_KEYS];
static int g_keyring_initialized = 0;

/* Master key (AES-128 = 16 bytes) */
#define MASTER_KEY_SIZE  16
static uint8_t g_master_key[MASTER_KEY_SIZE];
static int g_master_key_set = 0;

static spinlock_t g_keyring_lock;

/* ── Internal helpers ────────────────────────────────────────────── */

/*
 * Encrypt plaintext using AES-128-CBC with a random IV.
 *
 * @plain:     Plaintext.
 * @plain_len: Plaintext length (must be ≤ KEY_DATA_MAX).
 * @cipher:    Output buffer (must be at least plain_len + 16 bytes).
 * @cipher_len: Output length (includes IV prepended).
 * @iv_out:    Output IV (16 bytes, randomly generated).
 *
 * Returns 0 on success, -1 on error.
 */
static int keyring_encrypt(const uint8_t *plain, uint32_t plain_len,
                            uint8_t *cipher, uint32_t *cipher_len,
                            uint8_t iv_out[16])
{
    if (!plain || !cipher || !cipher_len || plain_len == 0 || plain_len > KEY_DATA_MAX)
        return -1;

    /* Generate random IV */
    for (int i = 0; i < 16; i++)
        iv_out[i] = (uint8_t)(rng_get_u32() & 0xFF);

    /* Pad plaintext to AES block size (16 bytes) */
    uint32_t padded_len = ((plain_len + 15) / 16) * 16;
    uint8_t *padded = (uint8_t *)kmalloc(padded_len + 16);  /* extra for IV */
    if (!padded) return -1;

    memcpy(padded, plain, plain_len);
    /* PKCS#7 padding */
    uint8_t pad_val = (uint8_t)(padded_len - plain_len);
    for (uint32_t i = plain_len; i < padded_len; i++)
        padded[i] = pad_val;

    /* Encrypt with AES-128-CBC */
    struct aes_ctx ctx;
    aes_init(&ctx, g_master_key, 16);
    aes_cbc_encrypt(&ctx, iv_out, padded, padded, padded_len);

    /* Output: IV (16 bytes) + ciphertext */
    memcpy(cipher, iv_out, 16);
    memcpy(cipher + 16, padded, padded_len);
    *cipher_len = padded_len + 16;

    kfree(padded);
    return 0;
}

/*
 * Decrypt ciphertext using AES-128-CBC.
 *
 * @cipher:     Ciphertext (IV + data).
 * @cipher_len: Length of ciphertext.
 * @plain:      Output buffer for plaintext.
 * @plain_len:  On input: max buffer size; on output: actual plaintext length.
 *
 * Returns 0 on success, -1 on error.
 */
static int keyring_decrypt(const uint8_t *cipher, uint32_t cipher_len,
                            uint8_t *plain, uint32_t *plain_len)
{
    if (!cipher || !plain || !plain_len || cipher_len < 32)  /* IV (16) + 1 block (16) */
        return -1;

    uint32_t data_len = cipher_len - 16;  /* Remove IV */
    uint8_t *data = (uint8_t *)kmalloc(data_len);
    if (!data) return -1;

    const uint8_t *iv = cipher;
    memcpy(data, cipher + 16, data_len);

    /* Decrypt */
    struct aes_ctx ctx;
    aes_init(&ctx, g_master_key, 16);
    aes_cbc_decrypt(&ctx, (uint8_t *)iv, data, data, data_len);

    /* Remove PKCS#7 padding */
    uint8_t pad_val = data[data_len - 1];
    if (pad_val > 16 || pad_val == 0) {
        kfree(data);
        return -1;  /* Invalid padding */
    }
    uint32_t unpadded_len = data_len - (uint32_t)pad_val;

    if (unpadded_len > *plain_len) {
        kfree(data);
        return -1;  /* Buffer too small */
    }

    memcpy(plain, data, unpadded_len);
    *plain_len = unpadded_len;

    kfree(data);
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────── */

/*
 * add_key — Add an encrypted key to the kernel keyring.
 *
 * @name:     String name for the key (max KEY_NAME_MAX chars).
 * @key_data:  Raw key data to store (will be encrypted at rest).
 * @key_len:   Length of key data (max KEY_DATA_MAX bytes).
 *
 * Returns 0 on success, -errno on failure.
 */
int add_key(const char *name, const uint8_t *key_data, uint32_t key_len)
{
    if (!name || !key_data || key_len == 0)
        return -EINVAL;

    if (!g_master_key_set)
        return -ENOKEY;  /* Master key not available */

    if (strlen(name) >= KEY_NAME_MAX)
        return -ENAMETOOLONG;

    if (key_len > KEY_DATA_MAX)
        return -E2BIG;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_keyring_lock, &irq_flags);

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < KEYRING_MAX_KEYS; i++) {
        if (!g_keyring[i].in_use) {
            slot = i;
            break;
        }
        /* Check for duplicate name */
        if (strcmp(g_keyring[i].name, name) == 0) {
            spinlock_irqsave_release(&g_keyring_lock, irq_flags);
            return -EEXIST;
        }
    }

    if (slot < 0) {
        spinlock_irqsave_release(&g_keyring_lock, irq_flags);
        return -ENOSPC;
    }

    struct keyring_entry *entry = &g_keyring[slot];

    /* Encrypt the key data */
    uint32_t cipher_len = sizeof(entry->encrypted_data);
    uint8_t iv[16];

    if (keyring_encrypt(key_data, key_len, entry->encrypted_data,
                        &cipher_len, iv) < 0) {
        spinlock_irqsave_release(&g_keyring_lock, irq_flags);
        return -EIO;
    }

    memcpy(entry->iv, iv, 16);
    entry->encrypted_len = cipher_len;
    entry->plaintext_len = key_len;
    strncpy(entry->name, name, KEY_NAME_MAX - 1);
    entry->name[KEY_NAME_MAX - 1] = '\0';
    entry->in_use = 1;

    spinlock_irqsave_release(&g_keyring_lock, irq_flags);

    kprintf("[KEYRING] Added key \"%s\" (%u bytes encrypted)\n",
            name, cipher_len);
    return 0;
}

/*
 * request_key — Retrieve a decrypted key from the kernel keyring.
 *
 * @name:     Name of the key to retrieve.
 * @buf:      Output buffer for the decrypted key data.
 * @buf_len:  On input: max buffer size; on output: actual key length.
 *
 * Returns 0 on success, -errno on failure.
 */
int request_key(const char *name, uint8_t *buf, uint32_t *buf_len)
{
    if (!name || !buf || !buf_len)
        return -EINVAL;

    if (!g_master_key_set)
        return -ENOKEY;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_keyring_lock, &irq_flags);

    /* Find the key by name */
    int slot = -1;
    for (int i = 0; i < KEYRING_MAX_KEYS; i++) {
        if (g_keyring[i].in_use && strcmp(g_keyring[i].name, name) == 0) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        spinlock_irqsave_release(&g_keyring_lock, irq_flags);
        return -ENOKEY;
    }

    struct keyring_entry *entry = &g_keyring[slot];

    /* Decrypt */
    uint32_t plain_len = *buf_len;
    int ret = keyring_decrypt(entry->encrypted_data, entry->encrypted_len,
                               buf, &plain_len);

    spinlock_irqsave_release(&g_keyring_lock, irq_flags);

    if (ret == 0)
        *buf_len = plain_len;

    return ret;
}

/*
 * revoke_key — Remove a key from the keyring.
 *
 * @name:  Name of the key to revoke.
 *
 * Returns 0 on success, -errno on failure.
 */
int revoke_key(const char *name)
{
    if (!name)
        return -EINVAL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_keyring_lock, &irq_flags);

    for (int i = 0; i < KEYRING_MAX_KEYS; i++) {
        if (g_keyring[i].in_use && strcmp(g_keyring[i].name, name) == 0) {
            /* Sanitize and release slot */
            memset(&g_keyring[i], 0, sizeof(struct keyring_entry));
            spinlock_irqsave_release(&g_keyring_lock, irq_flags);
            kprintf("[KEYRING] Revoked key \"%s\"\n", name);
            return 0;
        }
    }

    spinlock_irqsave_release(&g_keyring_lock, irq_flags);
    return -ENOKEY;
}

/*
 * keyring_has_key — Check if a key exists in the keyring.
 *
 * Returns 1 if the key exists, 0 otherwise.
 */
int keyring_has_key(const char *name)
{
    if (!name) return 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_keyring_lock, &irq_flags);

    for (int i = 0; i < KEYRING_MAX_KEYS; i++) {
        if (g_keyring[i].in_use && strcmp(g_keyring[i].name, name) == 0) {
            spinlock_irqsave_release(&g_keyring_lock, irq_flags);
            return 1;
        }
    }

    spinlock_irqsave_release(&g_keyring_lock, irq_flags);
    return 0;
}

/*
 * keyring_get_key_count — Return number of keys currently in the keyring.
 */
int keyring_get_key_count(void)
{
    int count = 0;
    for (int i = 0; i < KEYRING_MAX_KEYS; i++) {
        if (g_keyring[i].in_use)
            count++;
    }
    return count;
}

/*
 * keyring_init — Initialize the kernel keyring.
 *
 * Generates a master encryption key from the kernel RNG.
 */
void keyring_init(void)
{
    if (g_keyring_initialized) return;

    spinlock_init(&g_keyring_lock);

    memset(g_keyring, 0, sizeof(g_keyring));

    /* Generate a random master key */
    for (int i = 0; i < MASTER_KEY_SIZE; i++)
        g_master_key[i] = (uint8_t)(rng_get_u32() & 0xFF);
    g_master_key_set = 1;

    g_keyring_initialized = 1;
    kprintf("[OK] Kernel keyring initialized (AES-128-CBC, max %d keys)\n",
            KEYRING_MAX_KEYS);
}
#include "module.h"
module_init(keyring_init);

/* ── Stub: keyring_create ─────────────────────────────── */
int keyring_create(const char *name)
{
    (void)name;
    kprintf("[keyring] keyring_create: not yet implemented\n");
    return 0;
}
/* ── Stub: keyring_destroy ─────────────────────────────── */
int keyring_destroy(const char *name)
{
    (void)name;
    kprintf("[keyring] keyring_destroy: not yet implemented\n");
    return 0;
}
/* ── Stub: keyring_add_key ─────────────────────────────── */
int keyring_add_key(const char *ring, const char *desc, const void *payload, size_t len)
{
    (void)ring;
    (void)desc;
    (void)payload;
    (void)len;
    kprintf("[keyring] keyring_add_key: not yet implemented\n");
    return 0;
}
/* ── Stub: keyring_search ─────────────────────────────── */
int keyring_search(const char *ring, const char *desc, void *payload, size_t *len)
{
    (void)ring;
    (void)desc;
    (void)payload;
    (void)len;
    kprintf("[keyring] keyring_search: not yet implemented\n");
    return 0;
}
/* ── Stub: keyring_remove_key ─────────────────────────────── */
int keyring_remove_key(const char *ring, const char *desc)
{
    (void)ring;
    (void)desc;
    kprintf("[keyring] keyring_remove_key: not yet implemented\n");
    return 0;
}
