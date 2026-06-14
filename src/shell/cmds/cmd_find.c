/* cmd_find.c — Search for files matching a pattern, with -exec support */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"    /* fnmatch */
#include "types.h"

void cmd_find(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: find <pattern> [-exec <command> {} \\;]\n");
        kprintf("  pattern   Glob pattern to match file names\n");
        kprintf("  -exec     Execute a command for each matched file,\n");
        kprintf("            replacing {} with the file path\n");
        return;
    }

    char arg_copy[512];
    strncpy(arg_copy, args, 511);
    arg_copy[511] = '\0';

    /* Parse: pattern [ -exec command {} \; ] */
    char pattern[64];
    char exec_cmd[64] = {0};
    char exec_args[256] = {0};
    int has_exec = 0;

    /* Use strtok to parse tokens */
    char *tokens[16];
    int ntokens = 0;
    char *token = strtok(arg_copy, " ");
    while (token && ntokens < 16) {
        tokens[ntokens++] = token;
        token = strtok(NULL, " ");
    }

    if (ntokens == 0) return;

    /* First token is the pattern */
    strncpy(pattern, tokens[0], 63);
    pattern[63] = '\0';

    /* Look for -exec in remaining tokens */
    int exec_pos = -1;
    for (int i = 1; i < ntokens; i++) {
        if (strcmp(tokens[i], "-exec") == 0) {
            exec_pos = i;
            break;
        }
    }

    if (exec_pos > 0) {
        has_exec = 1;
        /* Collect command and args up to \; */
        int ci = 0;
        int ai = 0;
        int found_semicolon = 0;
        /* First token after -exec is the command name */
        if (exec_pos + 1 < ntokens) {
            strncpy(exec_cmd, tokens[exec_pos + 1], 63);
            exec_cmd[63] = '\0';
            ci = exec_pos + 2;
            /* Collect remaining args until \; */
            while (ci < ntokens) {
                if (strcmp(tokens[ci], ";") == 0 ||
                    (tokens[ci][0] == '\\' && tokens[ci][1] == ';')) {
                    found_semicolon = 1;
                    ci++;
                    break;
                }
                /* Add separator */
                if (ai > 0 && ai < 255) exec_args[ai++] = ' ';
                /* Copy token */
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

    /* List all files and match using fnmatch */
    char names[128][FS_MAX_NAME];
    int n = fs_list_names("/", 0, names, 128);

    int found = 0;
    for (int i = 0; i < n; i++) {
        if (fnmatch(pattern, names[i], 0) == 0) {
            kprintf("  /%s\n", names[i]);
            found++;

            if (has_exec) {
                /* Build command args with {} substitution */
                char cmdline[256];
                strncpy(cmdline, exec_cmd, sizeof(cmdline) - 1);
                cmdline[sizeof(cmdline) - 1] = '\0';

                /* If exec_args contains {}, substitute with path.
                 * We need to do the substitution in the args string since
                 * libc_shell_exec_cmd takes (cmd, args). */
                char subst_args[256];
                int sai = 0;
                int in_subst = 0;

                /* Build the args with {} replaced by the file path */
                for (int k = 0; exec_args[k] && sai < 254; k++) {
                    if (exec_args[k] == '{' && exec_args[k+1] == '}') {
                        /* Substitute file path */
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
    }
    if (!found) kprintf("No files matching '%s'\n", pattern);
}
