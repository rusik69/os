# ── Security / LSM modules — obj-m entries for security .ko files ──────────
#
# This file is included by src/modules/Makefile.modules.
# All entries use obj-m += to append to the global module list.

obj-m += kernel/smack.ko
obj-m += kernel/ima.ko
obj-m += kernel/ima_policy.ko
obj-m += kernel/ima_appraise.ko
obj-m += kernel/evm.ko
obj-m += kernel/ipe.ko
obj-m += kernel/keyring.ko
obj-m += kernel/yama.ko
