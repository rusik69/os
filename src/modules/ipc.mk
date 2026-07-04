# ── IPC modules — obj-m entries for ipc/ .ko files ──────────────────────────
#
# This file is included by src/modules/Makefile.modules.
# All entries use obj-m += to append to the global module list.

obj-m += ipc/shm.ko
obj-m += ipc/mqueue.ko
obj-m += ipc/semaphore.ko
