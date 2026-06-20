/* cmd_reboot.c — reboot command: restart the system
 *
 * Convenience alias for "shutdown -r now".
 * Syncs filesystem and stops services before rebooting.
 */

#include "shell_cmds.h"

/* Forward declaration — shutdown command handles the details */
extern void cmd_shutdown(const char *args);

void cmd_reboot(void)
{
    cmd_shutdown("-r now");
}
