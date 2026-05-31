#define KERNEL_INTERNAL
#include "types.h"
#include "cmdline.h"
#include "string.h"
#include "printf.h"

#define CMDLINE_MAX_PARAMS 64
#define CMDLINE_MAX_KEY    64
#define CMDLINE_MAX_VAL    256

static char raw_cmdline[1024];
static int num_params = 0;
static char keys[CMDLINE_MAX_PARAMS][CMDLINE_MAX_KEY];
static char vals[CMDLINE_MAX_PARAMS][CMDLINE_MAX_VAL];

void cmdline_init(const char *cmdline) {
    if (!cmdline) { raw_cmdline[0] = '\0'; return; }

    int len = (int)strlen(cmdline);
    if (len > (int)sizeof(raw_cmdline) - 1) len = sizeof(raw_cmdline) - 1;
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
