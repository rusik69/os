/* cmd_envdir.c — run command with environment variables */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"
#include "shell.h"

int cmd_envdir(int argc, char **argv)
{
    if (argc < 3) {
        kprintf("usage: envdir <key=value>... <command> [args...]\n");
        return 1;
    }

    /* Find the command: first arg without '=' is the command */
    int cmd_idx = 1;
    for (int i = 1; i < argc; i++) {
        if (!strchr(argv[i], '=')) {
            cmd_idx = i;
            break;
        }
        /* Set env var: split on first '=' */
        const char *eq = strchr(argv[i], '=');
        if (eq) {
            char key[64], val[256];
            int klen = (int)(eq - argv[i]);
            if (klen > 63) klen = 63;
            memcpy(key, argv[i], klen);
            key[klen] = '\0';
            int vlen = strlen(eq + 1);
            if (vlen > 255) vlen = 255;
            memcpy(val, eq + 1, vlen);
            val[vlen] = '\0';
            shell_var_set(key, val);
            kprintf("envdir: set %s='%s'\n", key, val);
        }
        cmd_idx = i + 1;
    }

    if (cmd_idx >= argc) {
        kprintf("envdir: no command specified\n");
        return 1;
    }

    /* Build args string and run */
    char args_buf[1024];
    int pos = 0;
    for (int i = cmd_idx; i < argc; i++) {
        if (pos > 0) args_buf[pos++] = ' ';
        int slen = strlen(argv[i]);
        if (pos + slen >= (int)sizeof(args_buf) - 1) break;
        memcpy(args_buf + pos, argv[i], (size_t)slen);
        pos += slen;
    }
    args_buf[pos] = '\0';

    kprintf("envdir: running '%s'\n", argv[cmd_idx]);
    shell_exec_cmd(argv[cmd_idx], args_buf);
    return 0;
}

void envdir_init(void)
{
    kprintf("[OK] cmd_envdir: environment setter ready\n");
}
