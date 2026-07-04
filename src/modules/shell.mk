# ── Userspace-sourced kernel modules — obj-m entries for shell/ .ko files ──
#
# This file is included by src/modules/Makefile.modules.
# These modules live under userspace/kmods/ and are compiled with the same
# MODULE_CFLAGS as kernel-source modules, but use a separate vpath rule.
#
# shell.ko is a multi-file module linking several component .o files.

obj-m += shell.ko
shell-objs := shell/shell shell/shell_vars shell/shell_cmd_table shell/editor shell/history_persist shell/job_control shell/script shell/syntax
