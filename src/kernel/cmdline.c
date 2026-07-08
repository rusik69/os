#define KERNEL_INTERNAL
#include "types.h"
#include "errno.h"
#include "cmdline.h"
#include "string.h"
#include "printf.h"
#include "cmos.h"
#include "lockdown.h"

#define CMDLINE_MAX_PARAMS 64
#define CMDLINE_MAX_KEY    64
#define CMDLINE_MAX_VAL    256

/* CMOS NVRAM offsets for kernel cmdline storage */
#define CMDLINE_NVRAM_OFFSET 0  /* offset 0..63 maps to CMOS reg 14..77 */
#define CMDLINE_NVRAM_LEN    64

static char raw_cmdline[1024];
static int num_params = 0;
static char keys[CMDLINE_MAX_PARAMS][CMDLINE_MAX_KEY];
static char vals[CMDLINE_MAX_PARAMS][CMDLINE_MAX_VAL];

void cmdline_init(const char *cmdline) {
    if (!cmdline) { raw_cmdline[0] = '\0'; return; }

    int len = (int)strlen(cmdline);
    if ((size_t)len > sizeof(raw_cmdline) - 1) len = sizeof(raw_cmdline) - 1;
    memcpy(raw_cmdline, cmdline, (size_t)len);
    raw_cmdline[len] = '\0';

    num_params = 0;
    const char *p = raw_cmdline;
    while (*p && num_params < CMDLINE_MAX_PARAMS) {
        /* Skip leading spaces */
        while (*p == ' ') p++;
        if (!*p) break;

        /* Parse key */
        const char *eq = p;
        while (*eq && *eq != '=' && *eq != ' ') eq++;

        int klen = (int)(eq - p);
        if (klen > CMDLINE_MAX_KEY - 1) klen = CMDLINE_MAX_KEY - 1;
        memcpy(keys[num_params], p, (size_t)klen);
        keys[num_params][klen] = '\0';

        if (*eq == '=') {
            /* Has value */
            const char *vstart = eq + 1;
            const char *vend = vstart;
            while (*vend && *vend != ' ') vend++;
            int vlen = (int)(vend - vstart);
            if (vlen > CMDLINE_MAX_VAL - 1) vlen = CMDLINE_MAX_VAL - 1;
            memcpy(vals[num_params], vstart, (size_t)vlen);
            vals[num_params][vlen] = '\0';
            p = vend;
        } else {
            vals[num_params][0] = '\0';
            p = eq;
        }
        num_params++;
    }

    if (num_params > 0) {
        kprintf("[OK] Kernel cmdline: %s\n", raw_cmdline);
    }

    /* Parse lockdown= boot parameter */
    const char *lockdown_val = cmdline_get("lockdown");
    if (lockdown_val) {
        if (strcmp(lockdown_val, "integrity") == 0) {
            lock_down(LOCKDOWN_INTEGRITY);
        } else if (strcmp(lockdown_val, "confidentiality") == 0) {
            lock_down(LOCKDOWN_CONFIDENTIALITY);
        } else {
            kprintf("[cmdline] lockdown: unknown value '%s' (expected 'integrity' or 'confidentiality')\n", lockdown_val);
        }
    }
}

int cmdline_has(const char *key) {
    for (int i = 0; i < num_params; i++) {
        if (strcmp(keys[i], key) == 0) return 1;
    }
    return 0;
}

const char *cmdline_get(const char *key) {
    for (int i = 0; i < num_params; i++) {
        if (strcmp(keys[i], key) == 0) return vals[i];
    }
    return NULL;
}

int cmdline_get_int(const char *key, int default_val) {
    const char *v = cmdline_get(key);
    if (!v || !*v) return default_val;
    int val = 0, sign = 1;
    const char *p = v;
    if (*p == '-') { sign = -1; p++; }
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    return val * sign;
}

const char *cmdline_raw(void) {
    return raw_cmdline;
}

/* ── CMOS NVRAM persistence ─────────────────────────────────── */

int cmdline_nvram_save(const char *cmdline) {
    if (!cmdline) return -EINVAL;
    uint8_t buf[CMDLINE_NVRAM_LEN];
    memset(buf, 0, CMDLINE_NVRAM_LEN);
    int len = (int)strlen(cmdline);
    if (len > CMDLINE_NVRAM_LEN - 1) len = CMDLINE_NVRAM_LEN - 1;
    memcpy(buf, cmdline, (size_t)len);
    buf[len] = '\0';
    /* Write to CMOS NVRAM */
    for (int i = 0; i < CMDLINE_NVRAM_LEN; i++) {
        cmos_nvram_write((uint8_t)(CMDLINE_NVRAM_OFFSET + i), buf[i]);
    }
    return 0;
}

int cmdline_nvram_restore(char *buf, int max_len) {
    if (!buf || max_len <= 0) return -EINVAL;
    uint8_t raw[CMDLINE_NVRAM_LEN];
    /* Read from CMOS NVRAM */
    for (int i = 0; i < CMDLINE_NVRAM_LEN; i++) {
        raw[i] = cmos_nvram_read((uint8_t)(CMDLINE_NVRAM_OFFSET + i));
    }
    raw[CMDLINE_NVRAM_LEN - 1] = '\0';
    int len = (int)strlen((const char *)raw);
    if (len > max_len - 1) len = max_len - 1;
    memcpy(buf, raw, (size_t)len);
    buf[len] = '\0';
    return len;
}

/* ── Stub: cmdline_parse ─────────────────────────────── */
static int cmdline_parse(const char *cmdline, void *callback)
{
    (void)cmdline;
    (void)callback;
    kprintf("[cmdline] cmdline_parse: not yet implemented\n");
    return 0;
}
/* ── Stub: cmdline_find_option ─────────────────────────────── */
static int cmdline_find_option(const char *cmdline, const char *option, char *val, int len)
{
    (void)cmdline;
    (void)option;
    (void)val;
    (void)len;
    kprintf("[cmdline] cmdline_find_option: not yet implemented\n");
    return 0;
}
