/* cmd_find.c — Search for files matching a pattern, with -exec support */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"    /* fnmatch */
#include "types.h"

void cmd_find(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: find <pattern> [-exec <command> {} ;]\n");
        kprintf("  pattern   Glob pattern to match file names\n");
        kprintf("  -exec     Execute a command for each matched file,\n");
        kprintf("            replacing {} with the file path\n");
        return;
    }

    char arg_copy[512];
    strncpy(arg_copy, args, 511);
    arg_copy[511] = '\0';

    /* Use strtok to parse tokens */
    char *tokens[16];
    int ntokens = 0;
    char *token = strtok(arg_copy, " ");
    while (token && ntokens < 16) {
        tokens[ntokens++] = token;
        token = strtok(NULL, " ");
    }

    if (ntokens == 0) return;

    /* Parse -exec <command> [args...] {} ... ; (if present) */
    char exec_cmd[64] = {0};
    char exec_args[256] = {0};
    int has_exec = 0;

    int exec_pos = -1;
    for (int i = 0; i < ntokens; i++) {
        if (strcmp(tokens[i], "-exec") == 0) {
            exec_pos = i;
            break;
        }
    }

    if (exec_pos >= 0) {
        has_exec = 1;
        if (exec_pos + 1 < ntokens) {
            strncpy(exec_cmd, tokens[exec_pos + 1], 63);
            exec_cmd[63] = '\0';
            int ci = exec_pos + 2;
            int ai = 0;
            int found_semicolon = 0;
            while (ci < ntokens) {
                if (strcmp(tokens[ci], ";") == 0 ||
                    (tokens[ci][0] == '\\' && tokens[ci][1] == ';')) {
                    found_semicolon = 1;
                    ci++;
                    break;
                }
                if (ai > 0 && ai < 255)
                    exec_args[ai++] = ' ';
                for (int k = 0; tokens[ci][k] && ai < 255; k++)
                    exec_args[ai++] = tokens[ci][k];
                ci++;
            }
            if (!found_semicolon) {
                kprintf("find: missing terminating \\; for -exec\n");
                return;
            }
        } else {
            kprintf("find: -exec requires a command\n");
            return;
        }
    }

    /* List all files once and match against every pattern token.
     * The shell glob-expands the command line (e.g. `find find*` becomes
     * `find findme1 findme2`), so treat every non -exec token as a pattern
     * (stop at -exec). */
    char names[128][FS_MAX_NAME];
    int n = fs_list_names("/", 0, names, 128);

    int found = 0;
    for (int i = 0; i < n; i++) {
        int matched = 0;
        for (int pi = 0; pi < ntokens && pi != exec_pos; pi++) {
            if (fnmatch(tokens[pi], names[i], 0) == 0) {
                matched = 1;
                break;
            }
        }
        if (!matched)
            continue;

        kprintf("  /%s\n", names[i]);
        found++;

        if (has_exec) {
            /* Build command args with {} substitution */
            char cmdline[256];
            strncpy(cmdline, exec_cmd, sizeof(cmdline) - 1);
            cmdline[sizeof(cmdline) - 1] = '\0';

            char subst_args[256];
            int sai = 0;
            for (int k = 0; exec_args[k] && sai < 254; k++) {
                if (exec_args[k] == '{' && exec_args[k + 1] == '}') {
                    char path_buf[64];
                    path_buf[0] = '/';
                    strncpy(path_buf + 1, names[i], 62);
                    path_buf[63] = '\0';
                    int plen = strlen(path_buf);
                    int pc = 0;
                    while (pc < plen && sai < 254)
                        subst_args[sai++] = path_buf[pc++];
                    k++; /* skip the '}' */
                } else {
                    subst_args[sai++] = exec_args[k];
                }
            }
            subst_args[sai] = '\0';

            libc_shell_exec_cmd(cmdline, subst_args);
        }
    }
    if (!found)
        kprintf("No files matching pattern\n");
}
