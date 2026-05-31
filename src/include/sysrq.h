#ifndef SYSRQ_H
#define SYSRQ_H

#include "types.h"

/*
 * SysRq (System Request) — emergency keyboard commands.
 *
 * Activated by pressing Alt + SysRq + <command-key>.
 * In a kernel-mode shell, any logged-in user can type:
 *   sysrq <command>
 *
 * Available commands:
 *   b — reboot immediately
 *   p — print registers
 *   t — print task list
 *   m — print memory info
 *   k — kill all tasks on current console
 *   i — send SIGKILL to all processes
 *   o — poweroff
 *   s — sync filesystems
 *   u — remount all filesystems read-only
 *   f — full OOM kill
 */

/* Handle a sysrq command character */
void sysrq_handle(char cmd);

/* Returns 1 if the given char is a valid sysrq command */
int sysrq_is_valid(char cmd);

/* Initialize sysrq */
void sysrq_init(void);

#endif
