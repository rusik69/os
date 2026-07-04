# ── Container runtime modules — obj-m entries for container/ .ko files ─────
#
# This file is included by src/modules/Makefile.modules.
# All entries use obj-m += to append to the global module list.

obj-m += container/runtime.ko
obj-m += container/config.ko
obj-m += container/state.ko
obj-m += container/ext.ko
obj-m += container/storage.ko
obj-m += container/network.ko
obj-m += container/image.ko
obj-m += container/orch.ko
obj-m += container/service_proxy.ko
obj-m += container/scheduler_policy.ko
obj-m += container/seccomp_notify.ko
obj-m += container/checkpoint.ko
obj-m += container/security_scan.ko
