/* cmd_lsmod.c — List loaded kernel modules (M22)
 *
 * Usage: lsmod
 *
 * Calls the SYS_QUERY_MODULE syscall to enumerate all loaded modules.
 */
#include "shell_cmds.h"
#include "syscall.h"
#include "libc.h"
#include "string.h"
#include "printf.h"

void cmd_lsmod(const char *args)
{
    (void)args;

    /* Query all modules by passing empty string */
    char buf[2048];
    buf[0] = '\0';

    int64_t written = (int64_t)libc_syscall(SYS_QUERY_MODULE,
                                             0,  /* empty name = list all */
                                             (uint64_t)(uintptr_t)buf,
                                             (uint64_t)sizeof(buf) - 1,
                                             0, 0);

    if (written < 0) {
        kprintf("lsmod: failed to query modules\n");
        return;
    }

    if (written == 0) {
        kprintf("No modules loaded.\n");
        return;
    }

    /* Ensure null termination */
    buf[sizeof(buf) - 1] = '\0';

    kprintf("Module              State  Refcount\n");
    kprintf("------------------  -----  --------\n");

    /* Parse and display the module list */
    char *line = buf;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';

        /* Format: "name state=%d ref=%d" */
        char mod_name[64];
        int state = -1, ref = 0;

        const char *sp = line;
        /* Extract module name (stops at space or ' state=') */
        int mi = 0;
        while (*sp && *sp != ' ' && mi < (int)sizeof(mod_name) - 1)
            mod_name[mi++] = *sp++;
        mod_name[mi] = '\0';

        /* Parse state= and ref= */
        while (*sp) {
            if (strncmp(sp, "state=", 6) == 0) {
                sp += 6;
                state = 0;
                while (*sp >= '0' && *sp <= '9') {
                    state = state * 10 + (int)(*sp - '0');
                    sp++;
                }
            } else if (strncmp(sp, "ref=", 4) == 0) {
                sp += 4;
                ref = 0;
                while (*sp >= '0' && *sp <= '9') {
                    ref = ref * 10 + (int)(*sp - '0');
                    sp++;
                }
            } else {
                sp++;
            }
        }

        const char *state_str = "?";
        switch (state) {
            case 2: state_str = "Live"; break;
            case 3: state_str = "Unload"; break;
            case 5: state_str = "Error"; break;
            default: break;
        }

        kprintf("%-18s  %-5s  %d\n", mod_name, state_str, ref);

        line = next;
    }
}
