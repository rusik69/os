#define KERNEL_INTERNAL
#include "module_signature.h"
#include "module.h"
#include "module_elf.h"
#include "sha256.h"
#include "printf.h"
#include "string.h"
#include "elf.h"
#include "errno.h"

/*
 * Module Signature Verification (Item 75)
 *
 * Verifies RSA+SHA256 signatures on loadable kernel modules before they
 * are loaded into the kernel.  Signed modules carry a .module_sig ELF
 * section containing a PKCS#1 v1.5 RSA-2048 signature of the module's
 * SHA-256 hash.
 *
 * The public key is shared with the SSH host key (rsa_key.h).  Module
 * authors sign modules using OpenSSL or a similar tool:
 *
 *   openssl dgst -sha256 -sign private.pem module.ko -out module.sig
 *   objcopy --add-section .module_sig=module.sig module.ko
 *
 * Enforcement modes:
 *   0 = warn-only (log but allow unsigned/invalid modules)
 *   1 = enforce  (reject unsigned or invalid modules)
 */

/* ── State ───────────────────────────────────────────────────────────── */

static int module_sig_enforce = 1;   /* default: enforce */
static int module_sig_initialized = 0;

/* ── RSA verify from SSH crypto subsystem ────────────────────────────── */

/* rsa_verify() checks that @sig is a valid PKCS#1 v1.5 RSA-2048 signature
 * over @hash (32-byte SHA-256 digest).  Returns 0 on success, -1 on failure.
 * The public key is the SSH host key embedded in rsa_key.h.
 */
extern int rsa_verify(const uint8_t *hash, const uint8_t *sig);

/* ── Initialisation ──────────────────────────────────────────────────── */

void module_sig_init(void)
{
    module_sig_initialized = 1;
    kprintf("[OK] Module signature verification (%s enforcement)\n",
            module_sig_enforce ? "with" : "without");
}

void module_sig_set_enforce(int enforce)
{
    module_sig_enforce = enforce ? 1 : 0;
}

int module_sig_get_enforce(void)
{
    return module_sig_enforce;
}

/* ── ELF section walking helpers ─────────────────────────────────────── */

/* Find an ELF section by name within raw ELF data.
 * Returns the section header index, or -1 if not found. */
static int elf_find_section_by_name(const uint8_t *data, uint64_t size,
                                     const char *name)
{
    if (!data || size < sizeof(struct elf64_header))
        return -1;

    const struct elf64_header *hdr = (const struct elf64_header *)data;

    /* We need the section name string table (shstrtab) */
    uint16_t shstrndx = hdr->e_shstrndx;
    uint64_t shoff    = hdr->e_shoff;
    uint16_t shentsz  = hdr->e_shentsize;
    uint16_t shnum    = hdr->e_shnum;

    if (shnum == 0 || shoff == 0 || shoff + (uint64_t)shnum * shentsz > size)
        return -1;

    /* Read shstrtab's section header */
    if ((uint64_t)shstrndx * shentsz + shentsz > size - shoff)
        return -1;

    const struct elf64_shdr *shstrtab_hdr =
        (const struct elf64_shdr *)(data + shoff + (uint64_t)shstrndx * shentsz);
    uint64_t shstrtab_off = shstrtab_hdr->sh_offset;
    uint64_t shstrtab_sz  = shstrtab_hdr->sh_size;

    if (shstrtab_off + shstrtab_sz > size)
        return -1;

    const char *shstrtab = (const char *)(data + shstrtab_off);
    size_t name_len = strlen(name);

    /* Scan all section headers */
    for (int i = 0; i < (int)shnum && i < 128; i++) {
        const struct elf64_shdr *sh =
            (const struct elf64_shdr *)(data + shoff + (uint64_t)i * shentsz);

        /* Get section name from shstrtab */
        const char *sname;
        if (sh->sh_name < shstrtab_sz) {
            sname = shstrtab + sh->sh_name;
        } else {
            continue;
        }

        if (strncmp(sname, name, name_len) == 0 && sname[name_len] == '\0')
            return i;
    }

    return -1;
}

/* ── Core verification ───────────────────────────────────────────────── */

int module_verify_elf(const uint8_t *elf_data, uint64_t elf_size)
{
    if (!module_sig_initialized || !elf_data || elf_size == 0)
        return -1;

    /* ── Step 1: find the .module_sig section ── */
    int sig_sec_idx = elf_find_section_by_name(elf_data, elf_size, ".module_sig");
    if (sig_sec_idx < 0) {
        /* No signature section */
        if (module_sig_enforce) {
            kprintf("[MOD_SIG] Module has no .module_sig section — "
                    "REJECTED (enforcement enabled)\n");
            return -EKEYREJECTED;
        }
        kprintf("[MOD_SIG] Module has no .module_sig section — "
                "ALLOWED (warn-only mode)\n");
        return 0;
    }

    /* ── Step 2: read the signature section ── */
    const struct elf64_header *hdr = (const struct elf64_header *)elf_data;
    uint64_t shoff   = hdr->e_shoff;
    uint16_t shentsz = hdr->e_shentsize;

    const struct elf64_shdr *sig_sh =
        (const struct elf64_shdr *)(elf_data + shoff +
                                    (uint64_t)sig_sec_idx * shentsz);

    uint64_t sig_offset = sig_sh->sh_offset;
    uint64_t sig_size   = sig_sh->sh_size;

    if (sig_offset + sig_size > elf_size) {
        kprintf("[MOD_SIG] .module_sig section extends past end of file\n");
        return -EKEYREJECTED;
    }

    if (sig_size < sizeof(struct module_sig_info)) {
        kprintf("[MOD_SIG] .module_sig too small (%llu bytes)\n",
                (unsigned long long)sig_size);
        return -EKEYREJECTED;
    }

    const struct module_sig_info *sig_info =
        (const struct module_sig_info *)(elf_data + sig_offset);

    if (sig_info->sig_len != MODULE_SIG_LEN) {
        kprintf("[MOD_SIG] Bad signature length (%u, expected %d)\n",
                (unsigned int)sig_info->sig_len, MODULE_SIG_LEN);
        return -EKEYREJECTED;
    }

    /* ── Step 3: compute SHA-256 hash of the module ELF content ──
     *
     * The hash covers the entire ELF file EXCEPT the .module_sig section.
     * This allows appending signatures without changing the signed content:
     *
     *   hash = SHA256(elf_data[0..sig_offset] || elf_data[sig_offset+sig_size..])
     */
    struct sha256_ctx ctx;
    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256_init(&ctx);

    /* Before the signature section */
    if (sig_offset > 0)
        sha256_update(&ctx, elf_data, sig_offset);

    /* After the signature section */
    uint64_t after_end = sig_offset + sig_size;
    if (after_end < elf_size)
        sha256_update(&ctx, elf_data + after_end, elf_size - after_end);

    sha256_final(digest, &ctx);

    /* ── Step 4: verify the RSA signature over the hash ── */
    int ret = rsa_verify(digest, sig_info->sig);
    if (ret != 0) {
        kprintf("[MOD_SIG] RSA signature verification FAILED — "
                "module rejected\n");
        return -EKEYREJECTED;
    }

    kprintf("[MOD_SIG] Module signature valid (SHA-256 + RSA-2048)\n");
    return 0;
}

/*
 * Verify a kernel module's signature using the parsed ELF context.
 * This is the integrated version called during module_elf_parse().
 */
int module_verify_sig(const uint8_t *module_data, size_t module_size,
                      const struct module_sig_info *sig_info, size_t sig_size)
{
    (void)sig_info;
    (void)sig_size;

    /* Use the raw ELF verification that finds sections itself */
    return module_verify_elf(module_data, module_size);
}
