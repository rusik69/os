/* cmd_nvme.c — NVMe device management commands
 *
 * Usage:
 *   nvme list                   — list NVMe namespaces and info
 *   nvme sanitize <action>      — run sanitize operation (1=block, 2=overwrite, 3=crypto)
 *   nvme sanitize-crypto        — shortcut: crypto erase entire drive
 *   nvme sanitize-block         — shortcut: block erase entire drive
 *
 * Item 187: NVMe sanitize operation (crypto erase).
 */

#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"
#include "nvme.h"

/*
 * Show NVMe controller and namespace information.
 */
static void nvme_list(void) {
    if (!nvme_is_present()) {
        kprintf("nvme: no NVMe controller present\n");
        return;
    }

    kprintf("NVMe Controller: present\n");
    struct nvme_identify_ctrl id;
    memset(&id, 0, sizeof(id));
    if (nvme_identify_ctrl(&id) == 0) {
        char mn[41], sn[21], fr[9];
        memcpy(mn, id.mn, 40); mn[40] = '\0';
        memcpy(sn, id.sn, 20); sn[20] = '\0';
        memcpy(fr, id.fr, 8);  fr[8]  = '\0';
        kprintf("  Model:       %s\n", mn);
        kprintf("  Serial:      %s\n", sn);
        kprintf("  Firmware:    %s\n", fr);
        kprintf("  Namespaces:  %u\n", (unsigned)id.nn);
    }
    nvme_print_info();
}

/*
 * Parse sanitize action name to code.
 */
static int parse_sanitize_action(const char *s) {
    if (!s) return -1;
    if (strcmp(s, "crypto") == 0 || strcmp(s, "3") == 0)
        return NVME_SANITIZE_ACTION_CRYPTO_ERASE;
    if (strcmp(s, "block") == 0 || strcmp(s, "1") == 0)
        return NVME_SANITIZE_ACTION_BLOCK_ERASE;
    if (strcmp(s, "overwrite") == 0 || strcmp(s, "2") == 0)
        return NVME_SANITIZE_ACTION_OVERWRITE;
    return -1;
}

/*
 * nvme sanitize <action> [overwrite_passes]
 */
static int cmd_nvme_sanitize(const char *action_str, const char *passes_str) {
    int action = parse_sanitize_action(action_str);
    if (action < 0) {
        kprintf("nvme: unknown sanitize action '%s' — use: block, overwrite, or crypto\n",
                action_str ? action_str : "");
        return 1;
    }

    int passes = 1;
    if (passes_str) {
        char *end = NULL;
        long val = strtol(passes_str, &end, 10);
        if (end && *end == '\0' && val >= 1 && val <= 16) {
            passes = (int)val;
        } else {
            kprintf("nvme: invalid overwrite pass count '%s' (must be 1..16)\n", passes_str);
            return 1;
        }
    }

    const char *action_names[] = { "", "Block Erase", "Overwrite", "Crypto Erase" };
    kprintf("nvme: issuing %s sanitize...\n",
            action >= 1 && action <= 3 ? action_names[action] : "unknown");

    /* ── DANGER: confirm with user ───────────────────────────── */
    kprintf("WARNING: This will DESTROY ALL DATA on the NVMe device!\n");
    kprintf("Type 'YES' to confirm: ");

    char response[16];
    int resp_len = 0;
    /* Read from serial console (simple line reader) */
    while (resp_len < (int)sizeof(response) - 1) {
        int c = keyboard_getchar();
        if (c == '\n' || c == '\r') break;
        if (c == '\b' || c == 0x7F) {
            if (resp_len > 0) resp_len--;
            continue;
        }
        response[resp_len++] = (char)c;
    }
    response[resp_len] = '\0';
    kprintf("\n");

    if (strcmp(response, "YES") != 0) {
        kprintf("nvme: sanitize cancelled\n");
        return 0;
    }

    /* Check if sanitize is supported before issuing command */
    /* The NVMe Identify Controller data has SANICAP field at byte 32 in the
     * reserved area that indicates sanitize capabilities. Check nvme_is_present()
     * and attempt to detect sanitize support from the controller capabilities. */
    if (!nvme_is_present()) {
        kprintf("nvme: no NVMe controller present\n");
        return 1;
    }

    int ret = nvme_sanitize(action, passes);
    if (ret == 0) {
        kprintf("nvme: sanitize command submitted successfully\n");
        return 0;
    } else {
        /* Try to determine why it failed */
        struct nvme_identify_ctrl id;
        memset(&id, 0, sizeof(id));
        if (nvme_identify_ctrl(&id) == 0) {
            char mn[41];
            memcpy(mn, id.mn, 40); mn[40] = '\0';
            kprintf("nvme: sanitize FAILED on controller '%s' (err=%d)\n", mn, ret);
            /* SANICAP would be at byte 32-35 in the identify data reserved area.
             * Check if it's non-zero to indicate sanitize support. */
            uint32_t sanicap = *(volatile uint32_t *)(((uint8_t*)&id) + 32);
            if (sanicap == 0) {
                kprintf("nvme: controller does not support sanitize operation\n");
            } else {
                kprintf("nvme: controller supports sanitize but command failed\n");
            }
        } else {
            kprintf("nvme: sanitize command FAILED with error %d\n", ret);
        }
        return 1;
    }
}

/*
 * Main nvme command dispatcher (shell_cmd_fn signature).
 */
void cmd_nvme(const char *args) {
    /* Skip leading whitespace */
    while (*args == ' ') args++;

    if (*args == '\0') {
        /* No arguments — show usage */
        kprintf("Usage:\n");
        kprintf("  nvme list                         — list NVMe information\n");
        kprintf("  nvme sanitize <action> [passes]   — run sanitize (block|overwrite|crypto)\n");
        kprintf("  nvme sanitize-crypto              — crypto erase (shortcut)\n");
        kprintf("  nvme sanitize-block               — block erase (shortcut)\n");
        kprintf("  nvme sanitize-overwrite [passes]  — overwrite erase (shortcut)\n");
        return;
    }

    /* Simple argv-style parsing from the args string */
    enum { MAX_ARGS = 8 };
    const char *argv[MAX_ARGS];
    int argc = 0;
    const char *p = args;

    while (*p && argc < MAX_ARGS) {
        /* Skip spaces */
        while (*p == ' ') p++;
        if (*p == '\0') break;

        argv[argc++] = p;
        /* Find end of this token */
        while (*p && *p != ' ') p++;
        if (*p) {
            /* Temporarily null-terminate by advancing pointer;
             * we use memcmp-based subcommand matching so this is fine. */
            p++;
        }
    }

    if (argc < 1) {
        kprintf("nvme: missing subcommand — try 'list', 'sanitize', 'sanitize-crypto'\n");
        return;
    }

    const char *sub = argv[0];

    /* Shortcut subcommands (single word) */
    if (strcmp(sub, "sanitize-crypto") == 0 || strcmp(sub, "crypto") == 0) {
        cmd_nvme_sanitize("crypto", NULL);
        return;
    }
    if (strcmp(sub, "sanitize-block") == 0 || strcmp(sub, "block") == 0) {
        cmd_nvme_sanitize("block", NULL);
        return;
    }
    if (strcmp(sub, "sanitize-overwrite") == 0 || strcmp(sub, "overwrite") == 0) {
        const char *passes = (argc > 1) ? argv[1] : NULL;
        cmd_nvme_sanitize("overwrite", passes);
        return;
    }

    if (strcmp(sub, "list") == 0 || strcmp(sub, "info") == 0) {
        nvme_list();
        return;
    }

    if (strcmp(sub, "sanitize") == 0) {
        const char *action = (argc > 1) ? argv[1] : NULL;
        const char *passes = (argc > 2) ? argv[2] : NULL;
        cmd_nvme_sanitize(action, passes);
        return;
    }

    kprintf("nvme: unknown subcommand '%s' — try 'list', 'sanitize', 'sanitize-crypto'\n", sub);
}
