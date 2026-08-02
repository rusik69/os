# ── Userspace-sourced kernel modules — obj-m entries for shell/ .ko files ──
#
# This file is included by src/modules/Makefile.modules.
# These modules live under userspace/kmods/ and are compiled with the same
# MODULE_CFLAGS as kernel-source modules, but use a separate vpath rule.
#
# shell.ko is a multi-file module linking several component .o files.

obj-m += shell.ko
# Multi-file module: core shell objects plus every builtin command in
# userspace/kmods/shell/cmds/, the cli_test component, and the DOS
# emulator (dos_exec is imported by cmd_dosbox).  Without these the
# symbols stay undefined in shell.ko and the module loader reports
# unresolved imports.
shell-objs := shell/shell shell/shell_vars shell/shell_cmd_table shell/editor shell/history_persist shell/job_control shell/script shell/syntax shell/cli_test \
    dos/dos_emu dos/dos_int21 dos/dos_ints dos/dos_load \
    $(patsubst userspace/kmods/shell/cmds/%.c,shell/cmds/%,$(wildcard userspace/kmods/shell/cmds/*.c))
