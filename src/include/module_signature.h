#ifndef MODULE_SIGNATURE_H
#define MODULE_SIGNATURE_H

#include "types.h"

/*
 * Module signature format.
 *
 * A signed kernel module ELF file contains a .module_sig section with
 * a 256-byte RSA-2048 PKCS#1 v1.5 signature over the SHA-256 hash of
 * the module's content (all loadable sections: .text, .rodata, .data,
 * .bss layout, .symtab, .strtab).
 *
 * The signature is computed using:
 *   1. SHA-256 hash of module content
 *   2. PKCS#1 v1.5 padding (EMSA-PKCS1-v1_5 with SHA-256 OID)
 *   3. RSA-2048 signature (modular exponentiation with private key)
 *
 * The kernel verifies using the embedded public key (shared with SSH host key).
 * See src/include/rsa_key.h for key data and src/kernel/ssh_crypto.c for the
 * RSA verification implementation.
 */

#define MODULE_SIG_LEN  256       /* RSA-2048 signature length in bytes */
#define MODULE_SIG_HASH_LEN 32    /* SHA-256 digest length */

/*
 * Module signature section header (embedded as .module_sig in ELF).
 * The raw 256-byte signature follows this header.
 */
struct module_sig_info {
    uint8_t  hash[MODULE_SIG_HASH_LEN];  /* SHA-256 of module content */
    uint8_t  sig[MODULE_SIG_LEN];        /* RSA-2048 PKCS#1 v1.5 signature */
    uint32_t sig_len;                    /* actual signature length (must be 256) */
    uint32_t hash_type;                  /* 0 = SHA-256 */
    uint8_t  version;                    /* signature format version (0) */
    uint8_t  reserved[15];               /* future use */
} __attribute__((packed));

_Static_assert(sizeof(struct module_sig_info) == MODULE_SIG_HASH_LEN + MODULE_SIG_LEN + 4 + 4 + 1 + 15,
               "module_sig_info size mismatch");

/*
 * ── Public API ──────────────────────────────────────────────────────────
 */

/* Initialize the module signature verification subsystem. */
void module_sig_init(void);

/*
 * Verify a kernel module's signature.
 *
 * @module_data   Pointer to the beginning of the module ELF image in memory.
 * @module_size   Total size of the module image in bytes.
 * @sig_info      Pointer to the module's .module_sig section data.
 * @sig_size      Size of the .module_sig section (must be >= sizeof(struct module_sig_info)).
 *
 * Returns 0 on success (signature valid), negative on failure:
 *   -EKEYREJECTED  Signature does not match
 *   -ENOPKG        No signature section found or invalid format
 *   -ENOENT        No public key loaded
 *   -ENOMEM        Out of memory
 */
int module_verify_sig(const uint8_t *module_data, size_t module_size,
                      const struct module_sig_info *sig_info, size_t sig_size);

/*
 * Verify a module's signature directly from raw ELF data.
 * Parses the ELF to find .module_sig, computes SHA-256, verifies RSA-2048.
 * This is the main entry point used by the ELF module loader.
 */
int module_verify_elf(const uint8_t *elf_data, uint64_t elf_size);

/*
 * Set the enforcement policy for module signature verification.
 * 0 = warn-only (log but allow), 1 = enforce (reject unsigned/invalid).
 */
void module_sig_set_enforce(int enforce);

/* Get current enforce mode. */
int module_sig_get_enforce(void);

/* ── PKCS#7 chain verification (S109) ────────────────────────────────── */

/* Add a trusted key (SHA-256 hash of the DER-encoded public key). */
int module_sig_add_trusted_key(const uint8_t key_hash[32]);

/* Clear all trusted keys. */
void module_sig_clear_trusted_keys(void);

/* Get the number of trusted keys. */
int module_sig_get_trusted_key_count(void);

#endif /* MODULE_SIGNATURE_H */
