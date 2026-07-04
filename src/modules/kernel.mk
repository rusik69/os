# ── Kernel infrastructure modules — obj-m entries for kernel/ .ko files ────
#
# This file is included by src/modules/Makefile.modules.
# All entries use obj-m += to append to the global module list.

obj-m += kernel/kprobes.ko
obj-m += kernel/ftrace.ko
obj-m += kernel/ftrace_stack.ko
obj-m += kernel/kexec.ko
obj-m += kernel/kdump.ko
obj-m += kernel/live_patch.ko
obj-m += kernel/uprobes.ko
obj-m += kernel/perf_events.ko
obj-m += kernel/perf_branch.ko
obj-m += kernel/hwlat_detector.ko
obj-m += kernel/kgdb_stub.ko
