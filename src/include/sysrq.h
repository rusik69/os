#ifndef SYSRQ_H
#define SYSRQ_H

#include "types.h"

/*
 * SysRq (System Request) — emergency keyboard commands.
 *
 * Activated by:
 *   - Writing a command character to /proc/sysrq-trigger
 *   - Pressing Alt + SysRq (PrintScreen) + <command-key>
 *   - Receiving the escape sequence on serial console
 *
 * Available commands:
 *   b — Emergency reboot
 *   c — Trigger crash (kdump testing)
 *   d — Show dmesg ring buffer
 *   e — Send SIGTERM to all tasks except init
 *   f — Full OOM kill
 *   h — Show this help
 *   i — Send SIGKILL to all tasks except init/self
 *   k — SAK (Secure Attention Key) — kill console processes
 *   l — Show all CPUs / stack backtrace
 *   m — Show memory information
 *   o — Emergency poweroff
 *   p — Show CPU registers
 *   s — Sync filesystems
 *   t — Show task list
 *   u — Remount all filesystems read-only
 *   w — Show blocked (D-state) tasks
 */

/* ── SysRq enable mask bits (mirrors Linux kernel.sysrq) ──────────── */
#define SYSRQ_ENABLE_ALL      0x0001  /* Enable all SysRq functions */
#define SYSRQ_ENABLE_SYNC     0x0002  /* Enable sync (s) */
#define SYSRQ_ENABLE_REMOUNT  0x0004  /* Enable remount ro (u) */
#define SYSRQ_ENABLE_SIGNAL   0x0008  /* Enable signal ops (e, i) */
#define SYSRQ_ENABLE_REBOOT   0x0010  /* Enable reboot (b) and poweroff (o) */
#define SYSRQ_ENABLE_DUMP     0x0020  /* Enable dumps (p, c, l, t, w, d) */
#define SYSRQ_ENABLE_OOM      0x0040  /* Enable OOM kill (f) */
#define SYSRQ_ENABLE_SAK      0x0080  /* Enable SAK (k) */
#define SYSRQ_ENABLE_HELP     0x0100  /* Enable help (h) */

/* Default: enable all (0 means disabled, -1 meaning all) */
#define SYSRQ_ENABLE_DEFAULT  0x01FF

/* Handle a sysrq command character */
void sysrq_handle(char cmd);

/* Returns 1 if the given char is a valid sysrq command */
int sysrq_is_valid(char cmd);

/* Initialize sysrq */
void sysrq_init(void);

/* ── SysRq enable mask (kernel.sysrq) ─────────────────────────────── */
int  sysrq_get_mask(void);
void sysrq_set_mask(int mask);

/* Check if a particular command character is allowed by current mask */
int sysrq_is_enabled(char cmd);

/* ── Serial trigger: called when a serial escape sequence is received ── */
/* Serial protocol: receive a NUL or break, followed by command char */
void sysrq_serial_trigger(char cmd);

#endif
