/* ima_template.c — IMA measurement-list template describer (D312 item 2)
 *
 * Implements the ima-ng ("new generation": digest payload + name) and
 * ima-sig (digest + name + signature field) template forms used to
 * serialize integrity measurements into the IMA measurement log.
 *
 * The active template is selectable by userspace via the sysfs file
 * /sys/kernel/security/ima_fmt (defaults to ima-ng).  ima.c calls
 * ima_template_render() when dumping each log entry.
 */

#define KERNEL_INTERNAL
#include "ima_template.h"

#include "errno.h"
#include "sha256.h"
#include "string.h"

/* Active template; defaults to ima-ng. */
static int ima_tpl_current = IMA_TPL_NG;

const char *ima_template_name(int id) {
    switch (id) {
    case IMA_TPL_NG:
        return "ima-ng";
    case IMA_TPL_SIG:
        return "ima-sig";
    default:
        break;
    }
    return NULL;
}

int ima_template_parse(const char *name, int len) {
    static const char ng[] = "ima-ng";
    static const char sig[] = "ima-sig";

    if (!name)
        return -EINVAL;
    if (len <= 0)
        return -EINVAL;

    if (len == (int)sizeof(ng) - 1 && memcmp(name, ng, (size_t)len) == 0)
        return IMA_TPL_NG;
    if (len == (int)sizeof(sig) - 1 && memcmp(name, sig, (size_t)len) == 0)
        return IMA_TPL_SIG;

    return -EINVAL;
}

int ima_template_current(void) {
    return ima_tpl_current;
}

int ima_template_select(const char *name, int len) {
    int id = ima_template_parse(name, len);
    if (id < 0)
        return id;

    ima_tpl_current = id;
    return 0;
}

/* Compute a type tag for a log entry (mirrors ima.c ima_buf_read). */
static const char *ima_tpl_type_str(int type, int appraised, int passed) {
    if (appraised)
        return passed ? "appraise-ok" : "appraise-fail";
    return (type == 1 /* IMA_FILE_EXEC */) ? "exec" : "read";
}

/* Hex-encode a digest into `hex` (2 chars per byte + NUL). */
static void ima_tpl_to_hex(const uint8_t *hash, char *hex, int hex_len) {
    static const char hex_chars[] = "0123456789abcdef";
    int i;
    for (i = 0; i < IMA_TPL_DIGEST_LEN && (i * 2 + 1) < hex_len; i++) {
        hex[i * 2] = hex_chars[(hash[i] >> 4) & 0x0F];
        hex[i * 2 + 1] = hex_chars[hash[i] & 0x0F];
    }
    hex[hex_len - 1] = '\0';
}

int ima_template_render(int id, const char *path, const uint8_t hash[IMA_TPL_DIGEST_LEN], int type,
                        int appraised, int passed, char *buf, int size) {
    const char *tpl_name;
    char hex[IMA_TPL_DIGEST_LEN * 2 + 1];
    const char *type_str;
    int n;

    if (!path || !hash || !buf || size <= 0)
        return -EINVAL;

    tpl_name = ima_template_name(id);
    if (!tpl_name)
        return -EINVAL;

    ima_tpl_to_hex(hash, hex, (int)sizeof(hex));
    type_str = ima_tpl_type_str(type, appraised, passed);

    if (id == IMA_TPL_SIG) {
        /* ima-sig: digest + name + signature field.  The measurement
         * record in this kernel carries no separate signature blob, so
         * the signature field is reported as unavailable. */
        n = snprintf(buf, (size_t)size, "%s %s sha256:%s sig:unavailable %s\n", type_str, tpl_name,
                     hex, path);
    } else {
        /* ima-ng: digest + name. */
        n = snprintf(buf, (size_t)size, "%s %s sha256:%s %s\n", type_str, tpl_name, hex, path);
    }

    return n;
}