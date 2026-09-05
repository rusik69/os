#ifndef IMA_TEMPLATE_H
#define IMA_TEMPLATE_H

/* IMA measurement-list template describer (D312 item 2).
 *
 * Templates control how a measurement entry is serialized in the
 * measurement log (see ima_buf_read in ima.c):
 *   ima-ng  — digest + name   (hash algorithm prefix + digest + path)
 *   ima-sig — digest + name + signature field
 * Selection is exposed to userspace via sysfs file "ima_fmt".
 */

/* IMA digest is always SHA-256 in this kernel (see ima.c IMA_DIGEST_SIZE). */
#define IMA_TPL_DIGEST_LEN 32

enum ima_template_id {
    IMA_TPL_NONE = 0, /* unknown / invalid */
    IMA_TPL_NG,       /* ima-ng  (digest + name)        */
    IMA_TPL_SIG,      /* ima-sig (digest + name + sig)  */
};

/* Return the canonical name for a template id, or NULL if unknown. */
const char *ima_template_name(int id);

/* Parse a template name of length `len` (not necessarily NUL terminated)
 * into a template id.  Returns the id, or -EINVAL if unknown. */
int ima_template_parse(const char *name, int len);

/* Return the currently active template id (defaults to ima-ng). */
int ima_template_current(void);

/* Select the active template by name.  Returns 0 on success, -EINVAL if
 * the name is unknown. */
int ima_template_select(const char *name, int len);

/* Render a single measurement entry [path, sha256 hash, type,
 * appraised, passed] into buf in the given template's ASCII form.
 * Returns bytes written (excluding NUL), or -EINVAL on bad arguments.
 * Entries are rendered as a single line of the form:
 *   <type> <tpl-name> sha256:<hex> [sig:<...>] <path>\n
 */
int ima_template_render(int id, const char *path, const uint8_t hash[IMA_TPL_DIGEST_LEN], int type,
                        int appraised, int passed, char *buf, int size);

#endif /* IMA_TEMPLATE_H */